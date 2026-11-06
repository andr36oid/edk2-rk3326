/** @file
  Placing kernel, initrd and FDT, ending boot services, entering Linux.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/LinuxBootLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define FDT_SLACK       SIZE_64KB   // room for bootargs, /memory, logo properties
#define PLACE_ATTEMPTS  32

/**
  Leaves boot services behind for good: caches cleaned and off, MMU off,
  x0 = Fdt, x1-x3 = 0, branch to Entry. AArch64/LinuxJump.S.
**/
VOID
EFIAPI
LinuxJumpToKernel (
  IN UINT64  Entry,
  IN UINT64  Fdt
  );

STATIC
VOID
FreeRange (
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Size
  )
{
  if ((Base != 0) && (Size != 0)) {
    gBS->FreePages (Base, EFI_SIZE_TO_PAGES (Size));
  }
}

/**
  Pages of Size at an Align boundary that the kernel can use: inside the
  RAM banks Linux gets and clear of every reserved region in Fdt. The
  allocator does not know about the DTB's reserved regions (U-Boot's logo
  buffer reaches past the scanned-out framebuffer), so a bad spot is held
  while the next one is tried.
**/
STATIC
EFI_STATUS
PlaceRange (
  IN  CONST VOID                *Fdt,
  IN  CONST LINUX_MEMORY_RANGE  *Memory,
  IN  UINTN                     MemoryCount,
  IN  UINT64                    Size,
  IN  UINT64                    Align,
  IN  CONST CHAR8               *What,
  OUT EFI_PHYSICAL_ADDRESS      *Base
  )
{
  EFI_PHYSICAL_ADDRESS  Held[PLACE_ATTEMPTS];
  UINTN                 HeldPages[PLACE_ATTEMPTS];
  EFI_PHYSICAL_ADDRESS  Raw;
  EFI_PHYSICAL_ADDRESS  Aligned;
  UINTN                 Pages;
  UINTN                 Count;
  UINTN                 I;
  CONST CHAR8           *Conflict;
  EFI_STATUS            Status;

  Pages  = EFI_SIZE_TO_PAGES (Size + Align);
  Status = EFI_OUT_OF_RESOURCES;
  for (Count = 0; Count < PLACE_ATTEMPTS; Count++) {
    Status = gBS->AllocatePages (AllocateAnyPages, EfiLoaderData, Pages, &Raw);
    if (EFI_ERROR (Status)) {
      break;
    }

    Aligned = ALIGN_VALUE (Raw, Align);
    if (!LinuxFdtRangeConflicts (Fdt, Memory, MemoryCount, Aligned, Size, &Conflict)) {
      // Give back the alignment slack on both sides.
      if (Aligned > Raw) {
        gBS->FreePages (Raw, EFI_SIZE_TO_PAGES (Aligned - Raw));
      }

      if (Raw + EFI_PAGES_TO_SIZE (Pages) > Aligned + ALIGN_VALUE (Size, EFI_PAGE_SIZE)) {
        gBS->FreePages (
               Aligned + ALIGN_VALUE (Size, EFI_PAGE_SIZE),
               EFI_SIZE_TO_PAGES (Raw + EFI_PAGES_TO_SIZE (Pages) - Aligned - ALIGN_VALUE (Size, EFI_PAGE_SIZE))
               );
      }

      *Base = Aligned;
      break;
    }

    DEBUG ((DEBUG_WARN, "Linux: %a at 0x%lx would hit %a, trying elsewhere\n", What, Aligned, Conflict));
    Held[Count]      = Raw;
    HeldPages[Count] = Pages;
    Status           = EFI_OUT_OF_RESOURCES;
  }

  for (I = 0; I < Count; I++) {
    gBS->FreePages (Held[I], HeldPages[I]);
  }

  return Status;
}

EFI_STATUS
EFIAPI
LinuxBootPrepare (
  IN  CONST LINUX_BOOT_PARAMS  *Params,
  OUT LINUX_BOOT_HANDOFF       *Handoff
  )
{
  LINUX_ARM64_IMAGE_INFO  Info;
  LINUX_FDT_FIXUP         Fixup;
  RETURN_STATUS           Ret;
  EFI_STATUS              Status;
  UINT64                  FdtBufSize;
  VOID                    *Fdt;

  if ((Params == NULL) || (Handoff == NULL) || (Params->Kernel == NULL) || (Params->Fdt == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Handoff, sizeof (*Handoff));

  Ret = LinuxArm64ParseHeader (Params->Kernel, Params->KernelSize, &Info);
  if (RETURN_ERROR (Ret)) {
    DEBUG ((DEBUG_ERROR, "Linux: not an arm64 Image (%r)\n", Ret));
    return EFI_LOAD_ERROR;
  }

  if ((FdtCheckHeader (Params->Fdt) != 0) || (FdtTotalSize (Params->Fdt) + FDT_SLACK > LINUX_FDT_MAX_SIZE)) {
    DEBUG ((DEBUG_ERROR, "Linux: the DTB is not a valid FDT (or larger than 2 MB)\n"));
    return EFI_LOAD_ERROR;
  }

  DEBUG ((
    DEBUG_INIT,
    "Linux: Image %lu bytes, text_offset 0x%lx, image_size 0x%lx, flags 0x%lx\n",
    (UINT64)Params->KernelSize,
    Info.TextOffset,
    Info.ImageSize,
    Info.Flags
    ));

  //
  // Kernel: text_offset above a 2 MB boundary (booti relocates it the same
  // way), image_size bytes free from there. The DTB itself (not yet fixed up)
  // decides what is reserved; the fixed-up one only adds the logo region,
  // which the DTB's drm-logo node already names when the logo is handed over.
  //
  Status = PlaceRange (
             Params->Fdt,
             Params->Fixup.Memory,
             Params->Fixup.MemoryCount,
             Info.TextOffset + Info.ImageSize,
             SIZE_2MB,
             "kernel",
             &Handoff->KernelBase
             );
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Handoff->KernelEntry = Handoff->KernelBase + Info.TextOffset;
  Handoff->KernelSize  = Info.ImageSize;
  CopyMem ((VOID *)(UINTN)Handoff->KernelEntry, Params->Kernel, Params->KernelSize);

  if ((Params->Initrd != NULL) && (Params->InitrdSize != 0)) {
    Status = PlaceRange (
               Params->Fdt,
               Params->Fixup.Memory,
               Params->Fixup.MemoryCount,
               Params->InitrdSize,
               EFI_PAGE_SIZE,
               "initrd",
               &Handoff->InitrdBase
               );
    if (EFI_ERROR (Status)) {
      goto Fail;
    }

    Handoff->InitrdSize = Params->InitrdSize;
    CopyMem ((VOID *)(UINTN)Handoff->InitrdBase, Params->Initrd, Params->InitrdSize);
  }

  //
  // FDT: 8-byte aligned and under 2 MB, and alone in its 2 MB block so the
  // kernel's cacheable block mapping of it covers nothing special.
  //
  FdtBufSize = ALIGN_VALUE (FdtTotalSize (Params->Fdt) + FDT_SLACK, EFI_PAGE_SIZE);
  Status     = PlaceRange (
                 Params->Fdt,
                 Params->Fixup.Memory,
                 Params->Fixup.MemoryCount,
                 FdtBufSize,
                 SIZE_2MB,
                 "FDT",
                 &Handoff->Fdt
                 );
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  Handoff->FdtSize = FdtBufSize;
  Fdt              = (VOID *)(UINTN)Handoff->Fdt;
  if (FdtOpenInto (Params->Fdt, Fdt, (INT32)FdtBufSize) != 0) {
    Status = EFI_LOAD_ERROR;
    goto Fail;
  }

  CopyMem (&Fixup, &Params->Fixup, sizeof (Fixup));
  Fixup.InitrdStart = Handoff->InitrdBase;
  Fixup.InitrdSize  = Handoff->InitrdSize;
  Ret               = LinuxFdtFixup (Fdt, FdtBufSize, &Fixup);
  if (RETURN_ERROR (Ret)) {
    DEBUG ((DEBUG_ERROR, "Linux: FDT fixup failed (%r)\n", Ret));
    Status = EFI_LOAD_ERROR;
    goto Fail;
  }

  DEBUG ((
    DEBUG_INIT,
    "Linux: kernel 0x%lx (entry 0x%lx), initrd 0x%lx+0x%lx, FDT 0x%lx (%u bytes)\n",
    Handoff->KernelBase,
    Handoff->KernelEntry,
    Handoff->InitrdBase,
    Handoff->InitrdSize,
    Handoff->Fdt,
    FdtTotalSize (Fdt)
    ));
  return EFI_SUCCESS;

Fail:
  DEBUG ((DEBUG_ERROR, "Linux: could not place the kernel (%r)\n", Status));
  LinuxBootFree (Handoff);
  return Status;
}

VOID
EFIAPI
LinuxBootFree (
  IN LINUX_BOOT_HANDOFF  *Handoff
  )
{
  if (Handoff == NULL) {
    return;
  }

  FreeRange (Handoff->KernelBase, Handoff->KernelEntry - Handoff->KernelBase + Handoff->KernelSize);
  FreeRange (Handoff->InitrdBase, Handoff->InitrdSize);
  FreeRange (Handoff->Fdt, Handoff->FdtSize);
  ZeroMem (Handoff, sizeof (*Handoff));
}

EFI_STATUS
EFIAPI
LinuxBootStart (
  IN EFI_HANDLE                ImageHandle,
  IN CONST LINUX_BOOT_HANDOFF  *Handoff
  )
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;
  UINTN                  Attempt;
  UINT64                 Entry = Handoff->KernelEntry;
  UINT64                 Fdt   = Handoff->Fdt;

  // The last thing that goes to the log before boot services end.
  DEBUG ((DEBUG_INIT, "Linux: starting the kernel at 0x%lx, FDT 0x%lx\n", Entry, Fdt));

  //
  // What the kernel reads before its MMU is on has to be in RAM, not only in
  // the cache: clean it to the point of coherency by address. LinuxJump.S
  // cleans the rest by set/way.
  //
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Handoff->KernelEntry, (UINTN)Handoff->KernelSize);
  if (Handoff->InitrdSize != 0) {
    WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Handoff->InitrdBase, (UINTN)Handoff->InitrdSize);
  }

  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Handoff->Fdt, (UINTN)Handoff->FdtSize);

  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  //
  // ExitBootServices fails when the map changed since GetMemoryMap (a timer
  // event allocating); the spec's answer is to fetch the map again and retry.
  //
  Map     = NULL;
  MapSize = 0;
  Status  = EFI_INVALID_PARAMETER;
  for (Attempt = 0; Attempt < 4; Attempt++) {
    Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      if (Map != NULL) {
        FreePool (Map);
      }

      MapSize += 8 * sizeof (EFI_MEMORY_DESCRIPTOR);
      Map      = AllocatePool (MapSize);
      if (Map == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }

      Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    Status = gBS->ExitBootServices (ImageHandle, MapKey);
    if (!EFI_ERROR (Status)) {
      // No DEBUG from here on: its library may use boot services.
      LinuxJumpToKernel (Entry, Fdt);
    }
  }

  if (Map != NULL) {
    FreePool (Map);
  }

  DEBUG ((DEBUG_ERROR, "Linux: ExitBootServices failed (%r)\n", Status));
  return Status;
}
