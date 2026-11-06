# Host tests for the board drivers. Plain gcc, no hardware, no edk2 build:
# the pure arithmetic headers are included directly and the real driver .c
# files are linked against mocks of the register access, I2C and FDT libraries.
#
#   make              build and run everything
#   make DTB=<file>   also run the PMIC test against a real device tree (default:
#                     the shipped Android DTB if it is on this machine)

MU    ?= ../../../../Mu_Basecore
RK    := ../..
SIL   := ../../../Silicium/SiliciumPkg
R36S  := ../../../../Platforms/GameConsole/r36sPkg
AND   := ../../../../Platforms/GameConsole/Andr36oidPkg
FDT   := $(MU)/MdePkg/Library/BaseFdtLib/libfdt/libfdt
DTS   ?= /andr36oid/src/kernel/gameconsole/r36s/arch/arm64/boot/dts/rockchip
UBOOT ?= /andr36oid/work/u-boot
DTB   ?= /andr36oid/src/device/gameconsole/r36s/BOOT/rk3326-r36s-android.dtb

CC     = gcc
CFLAGS = -std=gnu11 -O1 -g -fshort-wchar -funsigned-char -fno-strict-aliasing -Wall -Wno-unused-function -Wno-unused-variable \
         -I$(MU)/MdePkg/Include -I$(MU)/MdePkg/Include/X64 \
         -I$(RK)/RockchipPkg/Include -I$(AND)/Include -I$(SIL)/Include \
         -I$(RK)/RK3326Pkg/Library/GpioLib -I$(RK)/RK3326Pkg/Library/CruLib \
         -I$(RK)/RK3326Pkg/Library/RkI2cLib -I$(RK)/RK3326Pkg/Library/RkClockInitLib -I$(R36S)/Drivers/BoardInitDxe -I$(RK)/RockchipPkg/Library/RkPmicLibRk817 \
         -I$(R36S)/Library/BootKeysLib -I$(R36S)/Library/KeypadDeviceLib \
         -Imock

TESTS = test_math test_i2c test_cru test_pmic test_keys dump_keys test_clock test_board dump_clk

all: $(TESTS)
	@for t in test_math test_i2c test_cru test_clock test_board; do echo "== $$t"; ./$$t || exit 1; done
	@for a in 166 514 300 fail; do echo "== test_keys $$a"; ./test_keys $$a || exit 1; done
	@for m in --bad-id --no-ack --init-fail; do echo "== test_pmic $$m"; ./test_pmic $$m || exit 1; done
	@echo "== test_pmic with $(DTB)"; if [ -f $(DTB) ]; then ./test_pmic $(DTB) || exit 1; else echo "(no DTB, skipped)"; fi
	@echo "== key map against the kernel DTS"; if [ -d $(DTS) ]; then python3 -I check-keys-vs-dts.py $(DTS) ./dump_keys || exit 1; else echo "(no kernel tree, skipped)"; fi
	@echo "== clock tables against U-Boot"; if [ -d $(UBOOT) ]; then python3 -I check-clk-vs-uboot.py $(UBOOT) ./dump_clk || exit 1; else echo "(no U-Boot tree, skipped)"; fi
	@echo ALL OK

mock/fdt_shim.o: mock/fdt_shim.c
	$(CC) $(CFLAGS) -I$(FDT) -c -o $@ $<

LIBFDT = $(FDT)/fdt.c $(FDT)/fdt_ro.c $(FDT)/fdt_wip.c $(FDT)/fdt_strerror.c
COMMON = mock/mocks.c

test_math: test_math.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^

test_cru: test_cru.c $(COMMON) $(RK)/RK3326Pkg/Library/CruLib/CruLib.c
	$(CC) $(CFLAGS) -o $@ $^

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
