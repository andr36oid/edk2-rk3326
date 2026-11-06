/** @file
  Memory map for RK3326 handhelds, built at run time: the RAM size comes from
  the DDR init's record and the framebuffer from the VOP that U-Boot left
  scanning out, so one image fits 512 MB and 1 GB boards and every panel.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/PcdLib.h>
#include <Library/RkLogLib.h>
#include <Library/RkPlatformInfoLib.h>

#define FALLBACK_DRAM_SIZE  SIZE_512MB
#define DXE_HEAP_SIZE       0x05000000      // 80 MB
#define OPTEE_BASE          0x08400000
#define OPTEE_SIZE          0x01000000
#define BOOTLOADER_FDT_BASE 0x01F00000      // U-Boot's fdt_addr_r
#define BOOTLOADER_FDT_SIZE 0x00100000

STATIC EFI_MEMORY_REGION_DESCRIPTOR  mMap[24];
STATIC UINT8                         mCount;

STATIC
VOID
Add (
  IN CONST CHAR8           *Name,
  IN EFI_PHYSICAL_ADDRESS  Base,
  IN UINT64                Length,
  IN EFI_MEMORY_HOB_OPTION Hob,
  IN EFI_RESOURCE_TYPE     Resource,
  IN EFI_RESOURCE_ATTRIBUTE_TYPE Attributes,
  IN EFI_MEMORY_TYPE       Type,
  IN ARM_MEMORY_REGION_ATTRIBUTES Arm
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
  RK_FRAMEBUFFER_INFO   Fb;
  EFI_PHYSICAL_ADDRESS  RamTop;
  EFI_PHYSICAL_ADDRESS  FdBase    = FixedPcdGet64 (PcdFdBaseAddress);
  EFI_PHYSICAL_ADDRESS  FdEnd     = FdBase + FixedPcdGet32 (PcdFdSize);
  EFI_PHYSICAL_ADDRESS  StackBase = FixedPcdGet64 (PcdCPUCoresStackBase);
  EFI_PHYSICAL_ADDRESS  StackEnd  = StackBase + FixedPcdGet32 (PcdCPUCorePrimaryStackSize);
  EFI_PHYSICAL_ADDRESS  HeapEnd   = StackEnd + DXE_HEAP_SIZE;
  EFI_PHYSICAL_ADDRESS  FbStart   = 0;
  EFI_PHYSICAL_ADDRESS  FbEnd     = 0;
  BOOLEAN               HaveFb;

  HaveFb = !EFI_ERROR (RkGetActiveFramebuffer (&Fb));
  if (HaveFb) {
    FbStart = Fb.Base & ~(EFI_PHYSICAL_ADDRESS)EFI_PAGE_MASK;
    FbEnd   = ALIGN_VALUE (Fb.Base + (UINT64)Fb.Stride * Fb.Height, EFI_PAGE_SIZE);
  }

  RamTop = RkGetDramSize ();
  if (RamTop == 0) {
    RamTop = (FbEnd > FALLBACK_DRAM_SIZE) ? SIZE_1GB : FALLBACK_DRAM_SIZE;
  }

  // A framebuffer outside the upper RAM block (or past the end) is not ours to keep.
  if (HaveFb && ((FbStart < OPTEE_BASE + OPTEE_SIZE) || (FbEnd > RamTop))) {
    HaveFb = FALSE;
  }

  mCount = 0;

  // BL31 and its shared memory, then the kernel's ramoops buffer (region@110000
  // in our DTBs): both must survive UEFI untouched.
  Add ("BL31", 0x00000000, 0x00110000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK);
  Add ("Ramoops", 0x00110000, 0x000F0000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK);
  AddRam (0x00200000, RK_LOG_BASE);
  // The firmware log ring (this boot and the previous one).
  Add ("UEFI Log", RK_LOG_BASE, RK_LOG_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK);
  // Where U-Boot put the FDT it passed in x0; DXE reads it.
  Add ("Bootloader FDT", BOOTLOADER_FDT_BASE, BOOTLOADER_FDT_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK);
  Add ("UEFI FD", FdBase, FdEnd - FdBase, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK);
  AddRam (FdEnd, StackBase);
  Add ("UEFI Stack", StackBase, StackEnd - StackBase, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK);
  Add ("DXE Heap", StackEnd, DXE_HEAP_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, Conv, WRITE_BACK);
  AddRam (HeapEnd, OPTEE_BASE);
  Add ("OP-TEE", OPTEE_BASE, OPTEE_SIZE, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK);

  if (HaveFb) {
    AddRam (OPTEE_BASE + OPTEE_SIZE, FbStart);
    Add ("Display Reserved", FbStart, FbEnd - FbStart, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH);
    AddRam (FbEnd, RamTop);
  } else {
    AddRam (OPTEE_BASE + OPTEE_SIZE, RamTop);
  }

  // All of the SoC's registers (GIC, GRF, CRU, GPIO, VOP, MMC, ...).
  Add ("Peripherals", 0xFF000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, DEVICE);
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
