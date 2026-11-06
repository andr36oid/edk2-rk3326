/** @file
  Direct Linux handoff, the way U-Boot's booti does it: our kernels have
  CONFIG_EFI off, so no EFI stub. The arm64 Image is placed text_offset
  bytes above a 2 MB boundary, the FDT gets /chosen and /memory, boot
  services end, caches and MMU go off and the kernel is entered at the
  current exception level with x0 = FDT, x1-x3 = 0.

  The parsing and FDT parts (LinuxImage.c, LinuxFdt.c) use no boot
  services, so the host tests build them with gcc.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _LINUX_BOOT_LIB_H_
#define _LINUX_BOOT_LIB_H_

#include <Uefi.h>

#define LINUX_ARM64_MAGIC           0x644D5241   // "ARM\x64" at offset 56
#define LINUX_ARM64_HEADER_SIZE     64
#define LINUX_ARM64_DEFAULT_OFFSET  0x80000      // text_offset of kernels with image_size 0
#define LINUX_UIMAGE_MAGIC          0x27051956
#define LINUX_UIMAGE_HEADER_SIZE    64
#define LINUX_FDT_MAX_SIZE          SIZE_2MB     // arm64 booting.rst
#define LINUX_MAX_MEMORY_RANGES     8

typedef struct {
  UINT64    TextOffset;
  UINT64    ImageSize;     // what the kernel needs from its start, BSS included
  UINT64    Flags;
} LINUX_ARM64_IMAGE_INFO;

typedef struct {
  UINT64    Base;
  UINT64    Size;
} LINUX_MEMORY_RANGE;

//
// What LinuxFdtFixup writes. Everything optional is 0/NULL when unused.
//
typedef struct {
  CONST CHAR8                 *Bootargs;      // /chosen/bootargs
  UINT64                      InitrdStart;    // physical, InitrdSize 0 = none
  UINT64                      InitrdSize;
  CONST LINUX_MEMORY_RANGE    *Memory;        // /memory banks, MemoryCount 0 = keep the DTB's
  UINTN                       MemoryCount;
  //
  // Boot logo handover ("rockchip,drm-logo" reserved region + logo,*/video,*
  // on the display route nodes, uboot-inventory section 3). HandoverFdt is an
  // FDT U-Boot already fixed up (the one it passed us): its drm-logo region and
  // route properties are copied over. Without one, FbBase/FbSize (the live
  // framebuffer) only fill in the region.
  //
  CONST VOID                  *HandoverFdt;
  UINT64                      FbBase;
  UINT64                      FbSize;
} LINUX_FDT_FIXUP;

/**
  Read the arm64 Image header (Documentation/arm64/booting.rst).
**/
RETURN_STATUS
EFIAPI
LinuxArm64ParseHeader (
  IN  CONST VOID              *Image,
  IN  UINTN                   Size,
  OUT LINUX_ARM64_IMAGE_INFO  *Info
  );

/**
  Find the payload of a legacy U-Boot uImage (ramdisk.uimg). Returns
  RETURN_UNSUPPORTED when Buffer is not a uImage (use it as it is) and
  RETURN_VOLUME_CORRUPTED when the header is broken or the file is short.
**/
RETURN_STATUS
EFIAPI
LinuxUImagePayload (
  IN  CONST VOID  *Buffer,
  IN  UINTN       Size,
  OUT UINTN       *PayloadOffset,
  OUT UINTN       *PayloadSize
  );

/**
  Apply Fixup to an FDT that already sits in a buffer of BufferSize bytes
  (FdtOpenInto). Packs it at the end.
**/
RETURN_STATUS
EFIAPI
LinuxFdtFixup (
  IN OUT VOID                   *Fdt,
  IN     UINTN                  BufferSize,
  IN     CONST LINUX_FDT_FIXUP  *Fixup
  );

/**
  The /memory banks of an FDT (U-Boot writes the RAM it hands to Linux
  there). Returns how many were stored in Ranges.
**/
UINTN
EFIAPI
LinuxFdtGetMemory (
  IN  CONST VOID          *Fdt,
  OUT LINUX_MEMORY_RANGE  *Ranges,
  IN  UINTN               MaxRanges
  );

/**
  /chosen/bootargs of an FDT, or NULL.
**/
CONST CHAR8 *
EFIAPI
LinuxFdtGetBootargs (
  IN CONST VOID  *Fdt
  );

/**
  TRUE if [Base, Base + Size) overlaps a /reserved-memory region or a
  /memreserve/ entry of Fdt, or is not inside one of Memory[] (when given).
  Name gets the reserved node's name for the log.
**/
BOOLEAN
EFIAPI
LinuxFdtRangeConflicts (
  IN  CONST VOID                *Fdt,
  IN  CONST LINUX_MEMORY_RANGE  *Memory OPTIONAL,
  IN  UINTN                     MemoryCount,
  IN  UINT64                    Base,
  IN  UINT64                    Size,
  OUT CONST CHAR8               **Name OPTIONAL
  );

//
// Everything placed and fixed up, ready for LinuxBootStart.
//
typedef struct {
  EFI_PHYSICAL_ADDRESS    KernelBase;     // 2 MB aligned
  EFI_PHYSICAL_ADDRESS    KernelEntry;    // KernelBase + text_offset
  UINT64                  KernelSize;     // bytes reserved from KernelEntry
  EFI_PHYSICAL_ADDRESS    InitrdBase;
  UINT64                  InitrdSize;
  EFI_PHYSICAL_ADDRESS    Fdt;
  UINT64                  FdtSize;
} LINUX_BOOT_HANDOFF;

typedef struct {
  CONST VOID          *Kernel;          // Image file
  UINTN               KernelSize;
  CONST VOID          *Initrd;          // ramdisk payload (uImage header stripped), may be NULL
  UINTN               InitrdSize;
  CONST VOID          *Fdt;             // DTB file
  LINUX_FDT_FIXUP     Fixup;            // InitrdStart/Size are filled in here
} LINUX_BOOT_PARAMS;

/**
  Copy kernel, initrd and FDT to their final places (EfiLoaderData, clear of
  the DTB's reserved regions and inside its RAM banks) and fix up the FDT.
  Nothing irreversible: on error everything is freed.
**/
EFI_STATUS
EFIAPI
LinuxBootPrepare (
  IN  CONST LINUX_BOOT_PARAMS  *Params,
  OUT LINUX_BOOT_HANDOFF       *Handoff
  );

/**
  ExitBootServices, clean the caches to the point of coherency, MMU and
  caches off, jump. Returns only if boot services could not be ended.
**/
EFI_STATUS
EFIAPI
LinuxBootStart (
  IN EFI_HANDLE                ImageHandle,
  IN CONST LINUX_BOOT_HANDOFF  *Handoff
  );

/**
  Free what LinuxBootPrepare allocated (a failed or abandoned boot).
**/
VOID
EFIAPI
LinuxBootFree (
  IN LINUX_BOOT_HANDOFF  *Handoff
  );

#endif
