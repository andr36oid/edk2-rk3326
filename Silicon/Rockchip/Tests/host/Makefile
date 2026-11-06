# Host tests for the board drivers. Plain gcc, no hardware, no edk2 build:
# the pure arithmetic headers are included directly and the real driver .c
# files are linked against mocks of the register access, I2C and FDT libraries.
#
#   make              build and run everything
#   make DTB=<file>   also run the PMIC test against a real device tree (default:
#                     the shipped Android DTB if it is on this machine)
#   STRICT_KNOWN=1 make   also fail on the firmware bugs the suites know about
#                     (printed as "KNOWN BUG <id>" on every run, see mock/known.h)

MU    ?= ../../../../Mu_Basecore
RK    := ../..
SIL   := ../../../Silicium/SiliciumPkg
R36S  := ../../../../Platforms/GameConsole/r36sPkg
AND   := ../../../../Platforms/GameConsole/Andr36oidPkg
FDT   := $(MU)/MdePkg/Library/BaseFdtLib/libfdt/libfdt
DTS   ?= /andr36oid/src/kernel/gameconsole/r36s/arch/arm64/boot/dts/rockchip
UBOOT ?= /andr36oid/work/u-boot
DTB   ?= /andr36oid/src/device/gameconsole/r36s/BOOT/rk3326-r36s-android.dtb
KERNEL ?= /andr36oid/src/kernel/gameconsole/r36s
DTBOUT ?= /andr36oid/src/out/target/product/r36s/obj/KERNEL_OBJ/arch/arm64/boot/dts/rockchip

CC     = gcc
CFLAGS = -std=gnu11 -O1 -g -fshort-wchar -funsigned-char -fno-strict-aliasing -Wall -Wno-unused-function -Wno-unused-variable \
         -I$(MU)/MdePkg/Include -I$(MU)/MdePkg/Include/X64 \
         -I$(RK)/RockchipPkg/Include -I$(AND)/Include -I$(SIL)/Include \
         -I$(RK)/RK3326Pkg/Library/GpioLib -I$(RK)/RK3326Pkg/Library/CruLib \
         -I$(RK)/RK3326Pkg/Library/RkI2cLib -I$(RK)/RK3326Pkg/Library/RkClockInitLib -I$(AND)/Library/PowerCheckLib -I$(R36S)/Drivers/BoardInitDxe -I$(RK)/RockchipPkg/Library/RkPmicLibRk817 \
         -I$(R36S)/Library/BootKeysLib -I$(R36S)/Library/KeypadDeviceLib \
         -Imock -I$(RK)/RockchipPkg/Drivers/DesignWare/DwMmcHcDxe -I$(RK)/RockchipPkg/Drivers/DwUsbHostDxe

TESTS = test_math test_i2c test_cru test_dwmmc test_pmic test_keys dump_keys test_clock test_board dump_clk test_power test_usb dump_usb \
        test_memmap test_platinfo test_pei test_log test_reset test_saradc test_sdplat

# The r36s layout as the build gets it (r36s.toml [uefi_fd], r36s.dsc stack).
TOML       := ../../../../Resources/Configs/r36s.toml
DSC        := $(R36S)/r36s.dsc
FD_BASE    := $(shell sed -n '/^\[uefi_fd\]/,/^\[/s/^base *= *\([0-9A-Fa-fx]*\).*/\1/p' $(TOML))
FD_SIZE    := $(shell sed -n '/^\[uefi_fd\]/,/^\[/s/^size *= *\([0-9A-Fa-fx]*\).*/\1/p' $(TOML))
STACK_BASE := $(shell sed -n 's/.*PcdCPUCoresStackBase|\([0-9A-Fa-fx]*\).*/\1/p' $(DSC))
STACK_SIZE := $(shell sed -n 's/.*PcdCPUCorePrimaryStackSize|\([0-9A-Fa-fx]*\).*/\1/p' $(DSC))

all: $(TESTS)
	@for t in test_math test_i2c test_cru test_dwmmc test_clock test_board test_power; do echo "== $$t"; ./$$t || exit 1; done
	@for t in test_memmap test_platinfo test_pei test_log test_reset test_saradc test_sdplat; do echo "== $$t"; ./$$t || exit 1; done
	@for a in 166 514 300 fail; do echo "== test_keys $$a"; ./test_keys $$a || exit 1; done
	@for m in --bad-id --no-ack --init-fail --uboot; do echo "== test_pmic $$m"; ./test_pmic $$m || exit 1; done
	@echo "== test_pmic with $(DTB)"; if [ -f $(DTB) ]; then ./test_pmic $(DTB) || exit 1; else echo "(no DTB, skipped)"; fi
	@echo "== key map against the kernel DTS"; if [ -d $(DTS) ]; then python3 -I check-keys-vs-dts.py $(DTS) ./dump_keys || exit 1; else echo "(no kernel tree, skipped)"; fi
	@echo "== clock tables against U-Boot"; if [ -d $(UBOOT) ]; then python3 -I check-clk-vs-uboot.py $(UBOOT) ./dump_clk || exit 1; else echo "(no U-Boot tree, skipped)"; fi
	@echo "== test_usb with $(DTBOUT)"; if [ -d $(DTBOUT) ]; then ./test_usb $(DTBOUT) || exit 1; else ./test_usb || exit 1; fi
	@echo "== USB PHY values against the kernel"; if [ -d $(DTS) ]; then python3 -I check-usb-vs-kernel.py $(KERNEL) ./dump_usb || exit 1; else echo "(no kernel tree, skipped)"; fi
	@echo "== display"; $(MAKE) -s -C display || exit 1
	@echo ALL OK

mock/fdt_shim.o: mock/fdt_shim.c
	$(CC) $(CFLAGS) -I$(FDT) -c -o $@ $<

LIBFDT = $(FDT)/fdt.c $(FDT)/fdt_ro.c $(FDT)/fdt_wip.c $(FDT)/fdt_strerror.c
COMMON = mock/mocks.c

test_math: test_math.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^

test_cru: test_cru.c $(COMMON) $(RK)/RK3326Pkg/Library/CruLib/CruLib.c
	$(CC) $(CFLAGS) -o $@ $^

test_dwmmc: test_dwmmc.c $(COMMON) $(RK)/RockchipPkg/Drivers/DesignWare/DwMmcHcDxe/DwMmcMath.h
	$(CC) $(CFLAGS) -o $@ test_dwmmc.c $(COMMON)

test_i2c: test_i2c.c $(COMMON) $(RK)/RK3326Pkg/Library/RkI2cLib/RkI2cLib.c
	$(CC) $(CFLAGS) -o $@ $^

test_pmic: test_pmic.c $(COMMON) $(RK)/RockchipPkg/Library/RkPmicLibRk817/RkPmicLibRk817.c mock/fdt_shim.c $(LIBFDT)
	$(CC) $(CFLAGS) -I$(FDT) -o $@ $^

test_keys: test_keys.c $(COMMON) $(R36S)/Library/BootKeysLib/BootKeysLib.c $(R36S)/Library/KeypadDeviceLib/KeypadDeviceLib.c
	$(CC) $(CFLAGS) -o $@ $^

dump_keys: dump_keys.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TESTS) mock/*.o

.PHONY: all clean

test_clock: test_clock.c $(COMMON) $(RK)/RK3326Pkg/Library/RkClockInitLib/RkClockInitLib.c
	$(CC) $(CFLAGS) -o $@ $^

test_board: test_board.c $(COMMON) $(R36S)/Drivers/BoardInitDxe/BoardInit.c $(RK)/RK3326Pkg/Library/GpioLib/GpioLib.c mock/fdt_shim.c $(LIBFDT)
	$(CC) $(CFLAGS) -I$(FDT) -o $@ $^

dump_clk: dump_clk.c
	$(CC) $(CFLAGS) -o $@ $<

test_power: test_power.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^

test_usb: test_usb.c $(COMMON) $(RK)/RK3326Pkg/Library/RkUsbPlatformLib/RkUsbPlatformLib.c $(RK)/RockchipPkg/Drivers/DwUsbHostDxe/DwUsbFifo.h mock/fdt_shim.c $(LIBFDT)
	$(CC) $(CFLAGS) -I$(FDT) -I$(RK)/RK3326Pkg/Library/RkUsbPlatformLib -o $@ test_usb.c $(COMMON) $(RK)/RK3326Pkg/Library/CruLib/CruLib.c $(RK)/RK3326Pkg/Library/RkUsbPlatformLib/RkUsbPlatformLib.c mock/fdt_shim.c $(LIBFDT)

dump_usb: dump_usb.c
	$(CC) $(CFLAGS) -I$(RK)/RK3326Pkg/Library/RkUsbPlatformLib -o $@ $<

test_memmap: test_memmap.c $(COMMON) mock/known.h $(R36S)/Library/MemoryMapLib/MemoryMapLib.c $(TOML) $(DSC)
	@test -n "$(FD_BASE)" -a -n "$(FD_SIZE)" -a -n "$(STACK_BASE)" -a -n "$(STACK_SIZE)" || { echo "no FD/stack layout in $(TOML) or $(DSC)"; exit 1; }
	$(CC) $(CFLAGS) -I$(MU)/ArmPkg/Include -I$(R36S)/Library/MemoryMapLib \
	  -D_PCD_VALUE_PcdFdBaseAddress=$(FD_BASE)ULL -D_PCD_VALUE_PcdFdSize=$(FD_SIZE)U \
	  -D_PCD_VALUE_PcdCPUCoresStackBase=$(STACK_BASE)ULL -D_PCD_VALUE_PcdCPUCorePrimaryStackSize=$(STACK_SIZE)U \
	  -o $@ test_memmap.c $(COMMON)

test_platinfo: test_platinfo.c $(COMMON) mock/known.h uboot_ref_mem.c uboot_ref_mem.h $(RK)/RK3326Pkg/Library/RkPlatformInfoLib/RkPlatformInfoLib.c
	$(CC) $(CFLAGS) -o $@ test_platinfo.c $(COMMON) uboot_ref_mem.c $(RK)/RK3326Pkg/Library/RkPlatformInfoLib/RkPlatformInfoLib.c

test_pei: test_pei.c $(COMMON) $(RK)/RK3326Pkg/Library/PlatformPeiLib/PlatformPeiLib.c $(TOML)
	@test -n "$(FD_BASE)" -a -n "$(FD_SIZE)" || { echo "no FD layout in $(TOML)"; exit 1; }
	$(CC) $(CFLAGS) -I$(MU)/ArmPkg/Include -I$(MU)/EmbeddedPkg/Include -I$(MU)/MdeModulePkg/Include -I$(RK)/RK3326Pkg/Library/PlatformPeiLib \
	  -D_PCD_GET_MODE_64_PcdFvBaseAddress=$(FD_BASE)ULL -D_PCD_GET_MODE_32_PcdFvSize=$(FD_SIZE)U -o $@ test_pei.c $(COMMON)

test_log: test_log.c $(COMMON) mock/known.h $(RK)/RockchipPkg/Library/RkSerialPortLib/RkSerialPortLib.c $(RK)/RockchipPkg/Library/RkSerialPortLib/Font.h
	$(CC) $(CFLAGS) -I$(RK)/RockchipPkg/Library/RkSerialPortLib -o $@ test_log.c $(COMMON)

test_reset: test_reset.c $(COMMON) $(RK)/RK3326Pkg/Library/ResetSystemLib/ResetSystemLib.c
	$(CC) $(CFLAGS) -I$(MU)/ArmPkg/Include -I$(MU)/MdeModulePkg/Include -I$(RK)/RK3326Pkg/Library/ResetSystemLib -o $@ test_reset.c $(COMMON)

test_saradc: test_saradc.c $(COMMON) $(RK)/RK3326Pkg/Library/RkSaradcLib/RkSaradcLib.c
	$(CC) $(CFLAGS) -o $@ $^

test_sdplat: test_sdplat.c $(COMMON) mock/known.h $(RK)/RK3326Pkg/Library/RkSdmmcPlatformLib/RkSdmmcPlatformLib.c $(RK)/RK3326Pkg/Library/GpioLib/GpioLib.c
	$(CC) $(CFLAGS) -o $@ test_sdplat.c $(COMMON) $(RK)/RK3326Pkg/Library/RkSdmmcPlatformLib/RkSdmmcPlatformLib.c $(RK)/RK3326Pkg/Library/GpioLib/GpioLib.c
