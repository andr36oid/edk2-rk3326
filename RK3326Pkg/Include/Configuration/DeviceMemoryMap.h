#ifndef _DEVICE_MEMORY_MAP_H_
#define _DEVICE_MEMORY_MAP_H_

#include <Library/ArmLib.h>

#define MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT 128

/* Below flag is used for system memory */
#define SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES                               \
  EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |        \
      EFI_RESOURCE_ATTRIBUTE_TESTED | EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |     \
      EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |                               \
      EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |                         \
      EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |                            \
      EFI_RESOURCE_ATTRIBUTE_EXECUTION_PROTECTABLE

typedef enum { NoHob, AddMem, AddDev, HobOnlyNoCacheSetting, MaxMem } DeviceMemoryAddHob;

#define MEMORY_REGION_NAME_MAX_LENGTH 32

typedef struct {
  CHAR8                        Name[MEMORY_REGION_NAME_MAX_LENGTH];
  EFI_PHYSICAL_ADDRESS         Address;
  UINT64                       Length;
  DeviceMemoryAddHob           HobOption;
  EFI_RESOURCE_TYPE            ResourceType;
  EFI_RESOURCE_ATTRIBUTE_TYPE  ResourceAttribute;
  EFI_MEMORY_TYPE              MemoryType;
  ARM_MEMORY_REGION_ATTRIBUTES ArmAttributes;
} ARM_MEMORY_REGION_DESCRIPTOR_EX, *PARM_MEMORY_REGION_DESCRIPTOR_EX;

#define MEM_RES EFI_RESOURCE_MEMORY_RESERVED
#define MMAP_IO EFI_RESOURCE_MEMORY_MAPPED_IO
#define SYS_MEM EFI_RESOURCE_SYSTEM_MEMORY

#define SYS_MEM_CAP SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES
#define INITIALIZED EFI_RESOURCE_ATTRIBUTE_INITIALIZED
#define WRITE_COMBINEABLE EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE
#define UNCACHEABLE EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE

#define Reserv EfiReservedMemoryType
#define Conv EfiConventionalMemory
#define BsData EfiBootServicesData
#define BsCode EfiBootServicesCode
#define RtData EfiRuntimeServicesData
#define RtCode EfiRuntimeServicesCode
#define MmIO EfiMemoryMappedIO

#define DEVICE ARM_MEMORY_REGION_ATTRIBUTE_DEVICE
#define WRITE_THROUGH ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH
#define WRITE_THROUGH_XN ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH
#define WRITE_BACK ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define WRITE_BACK_XN ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define UNCACHED_UNBUFFERED ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED
#define UNCACHED_UNBUFFERED_XN ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED

static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
  /*                                                    EFI_RESOURCE_ EFI_RESOURCE_ATTRIBUTE_ EFI_MEMORY_TYPE ARM_REGION_ATTRIBUTE_
     MemLabel(32 Char.),  MemBase,    MemSize, BuildHob, ResourceType, ResourceAttribute, MemoryType, CacheAttributes
  */
 
 
 //--------------------- DDR --------------------- */
 
  {"TF-A",              0x00000000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP,  Reserv,  DEVICE},
  {"HLOS",              0x00010000, 0x01FF0000, AddMem, SYS_MEM, SYS_MEM_CAP,   Conv,   WRITE_BACK},
  {"UEFI FD",           0x02000000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP,   BsCode, WRITE_BACK},
  {"HLOS",              0x02200000, 0x06200000, AddMem, SYS_MEM, SYS_MEM_CAP,   Conv,   WRITE_BACK},
  {"TRUST",             0x08400000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP,  Reserv,  DEVICE},
  {"HLOS",              0x08800000, 0x35600000, AddMem, SYS_MEM, SYS_MEM_CAP,   Conv,   WRITE_BACK},
  {"Display Reserved",  0x3DE00000, 0x00800000, AddMem, MEM_RES, WRITE_THROUGH, MaxMem, WRITE_THROUGH},
  {"HLOS",              0x3E600000, 0x01A00000, AddMem, SYS_MEM, SYS_MEM_CAP,   Conv,   WRITE_BACK},

//--------------------- Register ---------------------
  {"PMU",              0xFF000000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"PMU-GRF",          0xFF010000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"GPIO0",            0xFF040000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"GICD",             0xFF131000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"GICC",             0xFF132000, 0x02000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"GICH",             0xFF134000, 0x02000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"GICV",             0xFF136000, 0x02000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"GRF",              0xFF140000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"UART2",            0xFF160000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"Watchdog",         0xFF1E0000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"RKTIMER",          0xFF210000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"GPIO1",            0xFF250000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"GPIO2",            0xFF260000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"GPIO3",            0xFF270000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"TSADC",            0xFF280000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"SARADC",           0xFF288000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"OTP",              0xFF290000, 0x04000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"CRU",              0xFF2B0000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"PMUCRU",           0xFF2BC000, 0x01000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"USB2PHY_GRF",      0xFF2C0000, 0x10000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"USB20_OTG",        0xFF300000, 0x40000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"SDMMC",            0xFF370000, 0x04000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},
  {"SDIO",             0xFF380000, 0x04000,  AddDev, MMAP_IO, UNCACHEABLE,  MmIO,   DEVICE},

  /* Terminator for MMU */
  { "Terminator", 0, 0, 0, 0, 0, 0, 0}};

#endif