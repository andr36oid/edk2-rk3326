ARCH              = arm64
TARGET            = aarch64-linux-gnu
CC                = clang
CFLAGS            = --target=$(TARGET)
OBJCOPY           = llvm-objcopy
KERNEL_FLAGS     ?= 0

all: BootShim.elf BootShim.bin

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) ${CFLAGS} -c $< -o $@ -DREQUIRES_KERNEL_HEADER=$(REQUIRES_KERNEL_HEADER) -DFD_BASE=$(FD_BASE) -DFD_SIZE=$(FD_SIZE) -DKERNEL_FLAGS=$(KERNEL_FLAGS)

BootShim.S:
