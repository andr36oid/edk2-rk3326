# UEFI hardware test 1: chainloaded UEFI starts Android (milestone M1)

Image: Mu-r36s.bin (a DEBUG build of this branch).
(Builds before 683b60f cannot see the SD card: their SD/eMMC and USB drivers never start.)
U-Boot still does the hardware setup and loads the UEFI from BOOT/uefi.bin. The UEFI then reads
BOOT itself (board init, SD driver, boot app), makes the same choices boot.ini makes, and starts
Linux directly. The display driver only adopts U-Boot's screen (default), it changes no register.
Writes to the card: U-Boot sets uefi.txt to "tried", and the UEFI writes BOOT/uefi-log.txt
(and uefi-log-prev.txt when the start before it died inside the UEFI). Variables are RAM-only.
One-shot: a hang costs one power cycle, the next start is a normal Android start.

## Install
BOOT partition of the test card mounted at <B>:
    andr36oid/testkit/install-uefi-test.sh <B> Mu-r36s.bin (a DEBUG build of this branch) once
To repeat a test: set uefi.txt back to "uefi=once" (plus any keys below).

## Test 1a: plain start (no keys held)
Expected on screen: U-Boot logo, then the white UEFI log text:
  "BoardInit: ..." (clocks already at target under U-Boot), "BoardInit: IO_VSEL sys 0x... pmu 0x..."
  "Display: adopt mode, U-Boot's screen stays as it is", "RkGop: 640x480 ..."
  "RkSdmmc: booted from SD", "CRU: mmc0 from GPLL 1200000000 Hz / 12 / 2 = 50000000 Hz",
  "RkSdmmc: SD at 0xff370000, 50000 kHz", card identification lines
  "andr36oid boot", "boot: bootargs: ...", "boot: starting Linux"
then the Android boot animation.
After Android is up (adb from a PC):
  - adb shell cat /proc/cmdline  ==  the "boot: bootargs:" line in BOOT/uefi-log.txt
    ==  the cmdline of a normal U-Boot start (save one before the test)
  - MemTotal in /proc/meminfo matches a normal start; no garbage between logo and animation
Useful results: BOOT/uefi-log.txt (copy it off the card), or a photo of the last screen of text.
Hang with no text at all = died before the log started (SEC or memory map).
If the boot menu shows instead: its message line says why; photo it, "Save log", then Power off.

## Test 1b: menu (uefi=once again)
Hold Select while powering on: the menu appears. Up/Down/A move and pick. Pick "Save log" (a status
line appears), then "Recovery": recovery starts. Hold FN instead: recovery starts directly.
Hold Start+Select: Android starts with androidboot.andr36oid_safe=1 in /proc/cmdline, no Magisk.

## Test 1c: display re-init (after 1a works)
uefi.txt:
    uefi=once
    display=reinit
    screen_dtb=rg351mp-uboot.dtb
The screen goes dark for about a second and comes back with the same text. Look for
"Dsi: PHY locked, lanes in stop state" and "Display: bring-up panel@0: Success".
If it stays dark: note whether the backlight is lit. Full steps: display/HW-TEST-STEPS.txt.

## If SD reads fail with DMA errors
Same test with a DEBUG build with PcdDwMmcForcePio=TRUE (SD/eMMC transfers by hand through the FIFO,
PcdDwMmcForcePio): if that one works, the fault is in the DMA path, not the clock or the pins.

## Log lines worth checking in BOOT/uefi-log.txt
"RK3326: DRAM <n> MB, bootloader FDT 0x1f00000 (<size> bytes)" (0 = U-Boot moved the FDT)
"RK3326: boot device 0x4/0, OP-TEE 0x8400000 +0x..." (after U-Boot the tags are gone; OP-TEE then comes from the 34 MB block or the 30 MB default)
"boot: U-Boot says hwrev=... ram_mb=... bootpart=..."

## USB (in every start now; usb/REPORT.md has the full steps)
Nothing plugged: "USB: DWC2 GSNPSID 0x4f54...", "DwUsbHost: started in N ms" (expect N < 400), the
start is otherwise unchanged. A USB keyboard through an OTG adapter, plugged before power-on:
port speed and a keyboard handle in the log; in the menu the number keys work (arrows/Enter on
boards with buttons come with the next boot app round). A FAT USB stick: Mass Storage BlockIo and a
FAT volume in the log. After Android starts: dmesg shows no dwc2 errors and USB works in Android.
D-28S with a charger in: "charger in on a shared port, leaving it in charge mode", still charging.
