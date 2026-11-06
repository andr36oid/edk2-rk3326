/** @file
  What SEC learned on the QEMU virt test device.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _QEMU_VIRT_PLATFORM_INFO_HOB_H_
#define _QEMU_VIRT_PLATFORM_INFO_HOB_H_

#include <Uefi.h>

#define QEMU_VIRT_RAM_BASE    0x40000000
#define QEMU_VIRT_LOG_BASE    0x41E00000      // same offset into RAM as RK_LOG_BASE
#define QEMU_VIRT_LOG_SIZE    0x00100000
//
// qemu -kernel puts its FDT after the kernel and initrd, at RAM + 128 MB
// for a raw image this small and no -initrd. Like U-Boot's FDT window on
// the RK3326 it is reserved, so SEC and DXE both find it there.
//
#define QEMU_VIRT_FDT_BASE    0x48000000
#define QEMU_VIRT_FDT_SIZE    0x00200000

typedef struct {
  UINT64                  RamBase;
  UINT64                  RamSize;
  EFI_PHYSICAL_ADDRESS    Fdt;            // from x0, 0 if none
  UINT32                  FdtSize;
  UINT32                  Reserved;
} QEMU_VIRT_PLATFORM_INFO;

extern EFI_GUID  gQemuVirtPlatformInfoHobGuid;

/**
  RAM (from the FDT's /memory, else 1 GB) and the FDT in its window.
  No state: works in SEC and DXE alike. QemuVirtInfoLib.
**/
VOID
EFIAPI
QemuVirtGetPlatformInfo (
  OUT QEMU_VIRT_PLATFORM_INFO  *Info
  );

#endif
