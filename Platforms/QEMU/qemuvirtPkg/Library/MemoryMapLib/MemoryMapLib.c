/** @file
  Memory map of the QEMU virt test device. Laid out like the RK3326 one
  (log ring, FD, stack, DXE heap at the same offsets into RAM) plus the two
  things the test needs kept: qemu's FDT and the BOOT disk image.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/PcdLib.h>
#include <Guid/QemuVirtPlatformInfoHob.h>

#define DXE_HEAP_SIZE  0x05000000      // 80 MB, as on the RK3326

STATIC EFI_MEMORY_REGION_DESCRIPTOR  mMap[16];
STATIC UINT8                         mCount;

STATIC
VOID
Add (
  IN CONST CHAR8                   *Name,
  IN EFI_PHYSICAL_ADDRESS          Base,
  IN UINT64                        Length,
  IN EFI_MEMORY_HOB_OPTION         Hob,
  IN EFI_RESOURCE_TYPE             Resource,
  IN EFI_RESOURCE_ATTRIBUTE_TYPE   Attributes,
  IN EFI_MEMORY_TYPE               Type,
  IN ARM_MEMORY_REGION_ATTRIBUTES  Arm
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  *R;

  if ((Length == 0) || (mCount >= ARRAY_SIZE (mMap))) {
    return;
  }

  R = &mMap[mCount++];
  AsciiStrCpyS (R->Name, sizeof (R->Name), Name);
  R->Address           = Base;
  R->Length            = Length;
  R->HobOption         = Hob;
  R->ResourceType      = Resource;
  R->ResourceAttribute = Attributes;
  R->MemoryType        = Type;
  R->ArmAttributes     = Arm;
}

STATIC
VOID
AddRam (
  IN EFI_PHYSICAL_ADDRESS  Start,
  IN EFI_PHYSICAL_ADDRESS  End
  )
{
  if (End > Start) {
    Add ("RAM Partition", Start, End - Start, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK);
  }
}

STATIC
VOID
BuildMap (
  VOID
  )
{
  QEMU_VIRT_PLATFORM_INFO  Info;
  EFI_PHYSICAL_ADDRESS     FdBase    = FixedPcdGet64 (PcdFdBaseAddress);
  EFI_PHYSICAL_ADDRESS     FdEnd     = FdBase + FixedPcdGet32 (PcdFdSize);
  EFI_PHYSICAL_ADDRESS     StackBase = FixedPcdGet64 (PcdCPUCoresStackBase);
  EFI_PHYSICAL_ADDRESS     StackEnd  = StackBase + FixedPcdGet32 (PcdCPUCorePrimaryStackSize);
  EFI_PHYSICAL_ADDRESS     HeapEnd   = StackEnd + DXE_HEAP_SIZE;
  EFI_PHYSICAL_ADDRESS     DiskBase  = FixedPcdGet64 (PcdMemDiskBase);
  EFI_PHYSICAL_ADDRESS     DiskEnd   = DiskBase + FixedPcdGet64 (PcdMemDiskSize);
  EFI_PHYSICAL_ADDRESS     RamTop;

  QemuVirtGetPlatformInfo (&Info);
  RamTop = Info.RamBase + Info.RamSize;
  mCount = 0;

  AddRam (QEMU_VIRT_RAM_BASE, QEMU_VIRT_LOG_BASE);
  Add ("UEFI Log", QEMU_VIRT_LOG_BASE, QEMU_VIRT_LOG_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK);
  //
  // SiliciumPkg's Sec clears "Display Reserved" and reads its descriptor
  // even when the map has none (uninitialised): one page, no display.
  //
  AddRam (QEMU_VIRT_LOG_BASE + QEMU_VIRT_LOG_SIZE, FdBase - EFI_PAGE_SIZE);
  Add ("Display Reserved", FdBase - EFI_PAGE_SIZE, EFI_PAGE_SIZE, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH);
  Add ("UEFI FD", FdBase, FdEnd - FdBase, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK);
  AddRam (FdEnd, StackBase);
  Add ("UEFI Stack", StackBase, StackEnd - StackBase, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK);
  Add ("DXE Heap", StackEnd, DXE_HEAP_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK);
  AddRam (HeapEnd, QEMU_VIRT_FDT_BASE);
  // qemu's FDT, the boot app reads it like U-Boot's on the RK3326.
  Add ("Bootloader FDT", QEMU_VIRT_FDT_BASE, QEMU_VIRT_FDT_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK);
  AddRam (QEMU_VIRT_FDT_BASE + QEMU_VIRT_FDT_SIZE, DiskBase);
  // The BOOT FAT image: never handed out, MemDiskDxe serves it as a disk.
  Add ("BOOT Disk", DiskBase, DiskEnd - DiskBase, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK);
  AddRam (DiskEnd, RamTop);

  // GIC, PL011, PL031, fw_cfg, virtio-mmio.
  Add ("Peripherals", 0x08000000, 0x08000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, DEVICE);
}

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR  **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount
  )
{
  if (mCount == 0) {
    BuildMap ();
  }

  *MemoryDescriptor      = mMap;
  *MemoryDescriptorCount = mCount;
}
