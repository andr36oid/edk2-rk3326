# Host tests for RkDisplayDxe: its arithmetic against U-Boot's own code
# (uboot_ref.c), and the whole bring-up and shutdown on a fake PX30 for
# every DTB, checked register by register against U-Boot's values.
# check-display-dtbs.py reads each DTB again in Python as a second opinion
# on the FDT parser; test_entry runs the DXE entry point (adopt, reinit,
# dark screen, screen_fdt, protocol notify) with faked boot services.
#
#   make                       sweeps + every DTB under $(BOOT)
#   make DTBS="a.dtb b.dtb"    other DTBs (e.g. Android DTBs from an image)
#   make log DTB=x.dtb         the driver's log for one panel

MU    ?= ../../../../../Mu_Basecore
RK    := ../../..
FDT   := $(MU)/MdePkg/Library/BaseFdtLib/libfdt/libfdt
DRV   := $(RK)/RockchipPkg/Drivers/RkDisplayDxe
BOOT  ?= /andr36oid/src/device/gameconsole/r36s/BOOT
DTBS  ?= $(sort $(shell find $(BOOT) -name '*.dtb' 2>/dev/null))
DTB   ?= $(firstword $(DTBS))

CC     = gcc
CFLAGS = -std=gnu11 -O1 -g -fshort-wchar -funsigned-char -fno-strict-aliasing -Wall -Wno-unused-function \
         -Wno-unused-variable -Wno-unused-but-set-variable -Wno-pointer-sign \
         -I$(MU)/MdePkg/Include -I$(MU)/MdePkg/Include/X64 -I$(RK)/RockchipPkg/Include \
         -I$(RK)/RK3326Pkg/Library/CruLib -I$(RK)/RK3326Pkg/Library/GpioLib -I$(DRV) -I../mock -I$(FDT)

DRVSRCS = $(DRV)/DisplayMath.c $(DRV)/DisplayFdt.c $(DRV)/Display.c $(DRV)/Vop.c $(DRV)/Dsi.c \
       $(DRV)/InnoPhy.c $(DRV)/Panel.c $(DRV)/Backlight.c \
       $(RK)/RK3326Pkg/Library/CruLib/CruLib.c $(RK)/RK3326Pkg/Library/CruLib/CruDisplay.c \
       $(RK)/RK3326Pkg/Library/GpioLib/GpioLib.c \
       $(FDT)/fdt.c $(FDT)/fdt_ro.c $(FDT)/fdt_wip.c $(FDT)/fdt_strerror.c $(FDT)/fdt_addresses.c $(FDT)/fdt_rw.c

# Entry point test: a 640x480 bootloader DTB and a 640x480 screen DTB
ENTRY_FDT    ?= $(BOOT)/Panels/Panel4/rg351mp-kernel.dtb
ENTRY_SCREEN ?= $(BOOT)/Devices/G350/rg351mp-uboot.dtb

all: test_display test_entry
	./test_display $(DTBS)
	python3 -I check-display-dtbs.py ./test_display $(DTBS)
	./test_entry $(ENTRY_FDT) $(ENTRY_SCREEN)
	@echo ALL OK

test_display: test_display.c uboot_ref.c fake_env.c $(DRVSRCS) uboot_ref.h fake_env.h $(DRV)/RkDisplay.h
	$(CC) $(CFLAGS) -o $@ test_display.c uboot_ref.c fake_env.c $(DRVSRCS)

test_entry: test_entry.c fake_env.c $(DRVSRCS) $(DRV)/RkDisplayDxe.c fake_env.h $(DRV)/RkDisplay.h
	$(CC) $(CFLAGS) -o $@ test_entry.c fake_env.c $(DRVSRCS)

log: test_display
	./test_display --log $(DTB)

clean:
	rm -f test_display test_entry

.PHONY: all clean log
