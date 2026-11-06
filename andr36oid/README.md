# andr36oid UEFI: a Mu-Silicium port for RK3326 handhelds

This branch is the work towards replacing U-Boot as the andr36oid boot system
(LineageOS 18.1 on the R36S and its clones, Rockchip RK3326 / PX30) with UEFI
firmware. It is built on Project Mu through Mu-Silicium. The older edk2 port on
`master` of this repository was the starting point. Its RK3326 code was moved
into the Mu layout and mostly rewritten.

Status: **not tested on hardware yet**. All of it builds, and every check that
does not need a console passes (host unit tests, QEMU runs, image checks).
The first hardware test is the chainload test in `testkit/SMOKE-TEST.md`.

## Contents

1. Why UEFI, and how it boots
2. Milestones
3. Layout in the tree
4. Memory map
5. What each part does, and why it is done that way
6. The boot app: boot.ini without U-Boot
7. Linux handoff
8. Building
9. Testing without hardware
10. Testing on hardware
11. Bugs found while checking the port
12. Open items
13. Commit history

---

## 1. Why UEFI, and how it boots

The shipped chain is

    rkbin DDR init + miniloader -> BL31 (trust.img) -> U-Boot (uboot.img) -> boot.ini -> booti Linux

U-Boot is a vendor fork with board code added over time (keys, PMIC, panel
init, charge screen, eMMC test, Kernel Tweaks). It is hard to change safely and
hard to test. The target chain is

    rkbin DDR init + miniloader -> BL31 -> UEFI (in the uboot.img slot)
                                             |- hardware init: clocks, PMIC, display, keys, USB
                                             |- andr36oid boot app (boot.ini decisions, boot menu)
                                             '- direct Linux handoff (the booti contract)

Linux is started the way U-Boot's `booti` starts it (x0 = FDT, MMU and
D-cache off), not through the EFI stub. The andr36oid kernel has
`CONFIG_EFI` off, and the recovery and resizer kernels are prebuilt 4.19.183
kernels that will not be rebuilt. A UEFI that needed the EFI stub would not be
able to start them.

Mu-Silicium was chosen over plain edk2 because it already carries the
ARM handheld and phone platform glue (SEC/PEI for an image loaded by another
bootloader, a simple framebuffer GOP, the build wrapper). This port
needed less glue as a result.

## 2. Milestones

- **M1: chainloaded.** U-Boot still does the hardware setup, then `boot.ini`
  loads `uefi.bin` from BOOT and `booti`s it (only when `uefi.txt` asks for
  it, one start at a time). The UEFI reads BOOT itself, makes the same choices
  `boot.ini` makes, and starts Linux. Done in code; hardware test pending.
- **M2: the UEFI does the hardware init itself.** I2C, RK817 PMIC, SARADC
  board id, clocks, pins, IO domains, display (VOP, DW-DSI, Inno D-PHY, panel
  init from the screen DTB, PWM backlight), charge and low-battery screens, USB
  host. Done in code; under U-Boot most of it only adopts what U-Boot left.
- **M3: replace U-Boot.** The firmware image packs as `uboot.img` (verified:
  repacking the shipped `uboot.img` with the same tool is byte-identical). Still
  needed: hardware bring-up without U-Boot, `mkimg.sh` and flasher
  integration, and a fallback plan.

## 3. Layout in the tree

    Silicon/Rockchip/RockchipPkg            shared Rockchip code
      Drivers/DesignWare/DwMmcHcDxe         SD/eMMC host (DW-MMC), reworked for PX30
      Drivers/DesignWare/RkSdmmcDxe         SD/eMMC platform glue
      Drivers/DwUsbHostDxe                  DWC2 USB host (from the RPi driver), OTG port as host
      Drivers/RkDisplayDxe                  VOP + DW-DSI + Inno D-PHY + panel + backlight
      Drivers/RkFbDxe, Library/RkGopLib     GOP on U-Boot's framebuffer or our own
      Library/RkSerialPortLib               UART + RAM log ring + on-screen log
      Library/RkPmicLibRk817                RK817 PMIC: rails, charger, gauge, power key
    Silicon/Rockchip/RK3326Pkg              the SoC
      Library/PlatformSecLib                keeps x0 (U-Boot's FDT) from the entry
      Library/PlatformPeiLib                board facts HOB (RAM, FDT, framebuffer, boot tags)
      Library/RkPlatformInfoLib             RAM size from PMUGRF, live VOP framebuffer, miniloader tags, OP-TEE region
      Library/CruLib, RkClockInitLib        clocks (PLLs, bus dividers, SD/eMMC, display, USB)
      Library/GpioLib, RkI2cLib, RkSaradcLib, RkSdmmcPlatformLib, RkUsbPlatformLib, ResetSystemLib
    Platforms/GameConsole/r36sPkg           the device: one image for every RK3326 console
      Drivers/BoardInitDxe                  IO domains, pins, LEDs, rumble off, Wi-Fi enable (first in APRIORI)
      Library/MemoryMapLib                  memory map computed at run time
      Library/BootPlatformLib               what the boot app needs from the board
      Library/BootKeysLib, KeypadDeviceLib  buttons by board family, keypad device for the menu
      Tools/                                check-image.py, check-depex.py, emulate-shim.py, pack-uboot-img.sh
    Platforms/GameConsole/Andr36oidPkg      the andr36oid boot logic (board independent)
      Application/Andr36oidBoot             the boot app (default boot option)
      Library/BootPolicyLib                 boot.ini's decisions as C, host tested
      Library/LinuxBootLib                  the booti handoff and DT fixups
      Library/PowerCheckLib                 low-battery shutdown, charge screen
    Platforms/QEMU/qemuvirtPkg              QEMU virt device to test the boot app and handoff
    Resources/Configs/r36s.toml             FD at 0x02000000, size 0xFF000, BootShim header
    Silicon/Rockchip/Tests/host             host unit tests for the silicon and board code
    tools/host-test, tools/qemu-test.sh     boot app host tests, QEMU suite
    andr36oid/                              this document, build script, hardware test kit

## 4. Memory map

The map is computed at run time from the RAM size and what is on screen,
because one image serves 512 MB, 1 GB and 2 GB consoles and both chainloaded
and native starts.

| Region | Range | Why |
|---|---|---|
| BL31 | 0 - 0x110000 | TF-A, never touched |
| ramoops | 0x110000 - 0x200000 | the kernel's crash log, kept across resets; the miniloader's tags sit at 0x1FE000 inside it |
| UEFI log | 0x01E00000 + 1 MB | RAM log ring, read back after a reset; mapped uncached so it survives a hang |
| FDT window | 0x01F00000 - 0x02000000 | U-Boot's FDT (x0); the screen DTB at 0x01F80000. x0 is only trusted inside this window |
| FD | 0x02000000 + 0xFF000 | the firmware itself (fits a uboot.img slot) |
| stack, DXE heap | after the FD | 80 MB heap |
| OP-TEE | 0x08400000 + 30 MB | from the miniloader's tag or BL32's parameter block, as U-Boot finds it |
| framebuffer | from the VOP | U-Boot's logo, kept while the screen shows it |
| RAM | the rest | up to 0xFF000000 at most |
| MMIO | 0xFF000000 + 16 MB | registers |

## 5. What each part does, and why it is done that way

### BootShim and the image

`Mu-r36s.bin` is a 0x100-byte BootShim with an arm64 Image header in front of
the FD. U-Boot's `booti` moves an image to the header's `text_offset` unless
it is already there. The header is set up so `booti 0x01ffff00` leaves the
image in place and the FD lands exactly on its base, 0x02000000. When the
miniloader loads it at 0x00200000 (the M3 path) the shim copies the FD up
and syncs the I-cache.

A review found that the first shim jumped into its own literal pool and, when
loaded at 0x02000000, copied over its own copy loop. Every hardware start
would have crashed. Two checks now run on every r36s build:
`check-image.py` checks the header and payload against the FD, and
`emulate-shim.py` runs the shim under Unicorn for each load address.

### SEC and PEI: board facts

`PlatformSecLib` keeps x0, which is U-Boot's FDT under `booti`, and only
trusts it inside the FDT window (alignment, magic and size are checked;
anything else is ignored). `PlatformPeiLib` puts the board facts in a HOB:

- RAM size, decoded from PMUGRF OS_REG2/3 the way U-Boot's
  `rockchip_sdram_size` decodes them. An all-zero record means "unknown"
  (the registers' reset value). The size is capped at 0xFF000000 like U-Boot's
  `SDRAM_MAX_SIZE`, so RAM never overlaps the registers.
- The live framebuffer, read back from VOP WIN1 (format, stride, size from the
  window or the display timing). Nothing is read while the VO power domain is
  off or its bus is idle (PMU_PWRDN_ST bit 13, PMU_BUS_IDLE_ST), since that
  read would hang the bus on a start without U-Boot.
- The miniloader's ATAGs at 0x1FE000: boot device and OP-TEE region. They are
  parsed exactly as U-Boot's `rk_atags.c` does, including its hash (a hash of 0
  is accepted, as U-Boot accepts it).
- OP-TEE as `param_parse_optee_mem` finds it: the TOS tag only with
  `flags == 1`, else the legacy `tos_param_t` block at DDR + 34 MB (16-bit
  checksum, flags 1). Whole pages only, 1 to 64 MB. The default is 30 MB at
  0x08400000. The first default was 16 MB, which would have handed 14 MB of
  OP-TEE's memory to Linux. Kernel logs on real consoles show 32 MB missing
  from RAM (2 MB at 0 + 30 MB OP-TEE), which confirms the 30 MB.

### Logging

There is no serial console on these devices, so the log goes to three places:

- the UART, with a bounded wait so a dead UART cannot stall the boot;
- a RAM ring at 0x01E00000, saved by the boot app to `BOOT/uefi-log.txt`.
  The previous start's ring is kept as `uefi-log-prev.txt` only when that start
  died inside the firmware;
- the screen, drawn over U-Boot's logo or our own framebuffer from SEC on. It
  handles 16, 24 (U-Boot's logo) and 32 bpp. When the display driver moves to
  a new framebuffer, the newest lines are drawn again.

RELEASE builds keep ERROR, WARN and INIT lines and the boot app's own lines,
so a RELEASE log still shows the board facts and the boot decisions.

### Board init (BoardInitDxe, RkClockInitLib)

Runs first in DXE (APRIORI). It does what U-Boot does in C:

- APLL 600 MHz, GPLL 1200 MHz and the bus dividers;
- IO domain voltages (3.3 V like U-Boot), button and eMMC pins;
- rumble off, LEDs, Wi-Fi enable.

Under U-Boot it only fixes clocks that are not at their target, plus the IO
domains; `PcdRkBoardInitForce` forces the rest.

### SD and eMMC (DwMmcHcDxe)

The DW-MMC driver came from another Rockchip port and was reworked for PX30:

- the internal DMA (IDMAC) is enabled; it never was before;
- eMMC multi-block reads now send a stop;
- every wait has a bound;
- clock updates happen in the order the controller needs;
- a descriptor leak and an SD PIO overrun are fixed;
- the clocks and resets are now PX30's. The old code was RK3568's and would
  have left the SD clock dead;
- the card-detect polarity is fixed (it was inverted).

`PcdDwMmcForcePio` builds a PIO-only variant to tell DMA faults from clock or
pin faults on hardware.

### PMIC (RkPmicLibRk817)

- **Rails.** The rail enables are saved to POWER_EN_SAVE0/1, which the kernel's
  reboot notifier restores from. After a chainload start U-Boot's saved copy is
  kept, not overwritten.
- **Voltages.** Only fixed rails (min == max) get a voltage written, as in
  U-Boot. Writing a ranged rail's minimum put the D-28S boost at 4.7 V, below
  USB's 4.75 V.
- **Charger limits.** They match U-Boot: USB_CTRL 0xD0, 450 mA input without a
  charger and 1500 mA with one, charge current code 1.
- **Fuel gauge.** A native start sets what U-Boot's `rk817_fg_init` sets
  (instant voltage, no discharge current limit, gauge on).

### Power at start (PowerCheckLib)

This is U-Boot's charge animation logic:

- low-battery shutdown, and a charge screen when started by plugging in a
  charger;
- reboot reasons that U-Boot clears (normal, loader, recovery, fastboot,
  charging, ums) never show the charge screen, and the register is written
  back to NORMAL;
- below 3105 mV with a charger in, a reboot charges first and then boots by
  itself, like U-Boot's `charge_extrem_low_power`;
- the DC jack is read on GPIO0_B3, as U-Boot does.

### Display (RkDisplayDxe)

There are two modes:

- **Adopt** (default) keeps U-Boot's screen and changes no register.
- **Reinit** (a PCD, or `display=reinit` from the chainload hook) brings up the
  VOP, DW-DSI, Inno D-PHY, panel and PWM backlight from the screen DTB.

The panel init sequence and timings come from the same screen DTBs U-Boot
uses. The maths is tested against U-Boot's code and against 94 DTBs. On
shutdown, a panel rail stays on when another device uses it.

### USB host (DwUsbHostDxe, RkUsbPlatformLib)

The OTG port runs as a host at every start, so a USB keyboard works in the
menu and a USB stick can be a boot source. The DWC2 driver is ported from the
Raspberry Pi platform.

`RkUsbPlatformLib` handles PD_USB, the clocks, the PHY values (taken from the
kernel), the resets, and the 5 V supply as each board DTB describes it:

- EE boards switch `vcc_host` on GPIO3_A4, with the polarity taken from
  `enable-active-high`, as the kernel does.
- The D-28S shares the port with the charger. Its charger-detect pin reads
  "in" with nothing plugged, so its DT asks for pin AND PMIC (the kernel's
  rule). Before this fix the port stayed in charge mode for good.
- OTG power comes on as boost, then the PMIC OTG switch, in the kernel's order.
- R36S and EE DTs mark the boost and OTG switch boot-on. A start without
  U-Boot turns those two on, so the port has 5 V.

### Driver dispatch guard (check-depex.py)

Mu's `NonCoherentDmaLib` pulls in `IoMmuLib`. On AARCH64 that library's depex
waits for `gEdkiiIoMmuProtocolGuid`, which nothing on this SoC installs. As a
result the SD/eMMC and USB host drivers were never started, and the build gave
no sign of it. The port now uses `IoMmuLibNull`; our DMA mapping never calls
the IOMMU.

Two more problems hid it. Mu's incremental build keeps a `.depex` file that a
module no longer has, and the FDF packs it anyway. So `andr36oid/build.sh`
deletes them before building.

`check-depex.py` then reads `FVMAIN.Fv` from the built image and walks the DXE
dispatch, honouring the a priori list and the "no depex means all
architectural protocols" rule. The build fails if any driver could never start
(currently 45 drivers, 27 a priori).

## 6. The boot app: boot.ini without U-Boot

`Andr36oidBoot` is the default boot option. It reproduces `boot.ini` and
U-Boot's board code as C, so the same card starts the same way either way.

- **Board facts.** From U-Boot's command line when chainloaded (`hwrev`,
  serial, RAM, `bootpart`), else derived as U-Boot would:
  - board family from the SARADC;
  - RAM from the HOB;
  - the serial number U-Boot really produces on these consoles. Vendor
    storage is empty and eFuse reading is off, so U-Boot takes 8 bytes from
    an unseeded `rand()`, which gives `c3d9b8674f4b94f6` on every console.
    Keeping it means adb and the USB gadget see the same device whichever way
    it started.
- **Finding BOOT.** It uses U-Boot's `bootpart`, else U-Boot's own rule: the
  SD card's BOOT if it has `boot.ini`, else the eMMC's. An eMMC install's data
  card keeps `Image` but has no `boot.ini`, so it must not win. A USB stick
  counts only with Image, ramdisk, DTB and boot.ini.
- **Decisions (BootPolicyLib, host tested against boot.ini):**
  - keys held at power-on (Fn: recovery; Select: menu; Start+Select: safe
    mode);
  - `recovery.txt`, the one-shot eMMC test, device profiles;
  - Kernel Tweaks (`kernel-tweaks.txt`, `kt_args`);
  - DTB choice, kernel command line and `androidboot.*` arguments.
- **Power check.** After U-Boot it runs first; on a start without U-Boot it
  runs once BOOT's screen DTB has brought the panel up, so the charge and
  low-battery screens are visible (U-Boot's order too).
- **Menu.** It shows on request or when something fails. It can start Android,
  recovery or a USB stick, save the log, or power off. It works with buttons,
  a USB keyboard or a mouse.
- **Screen and USB power.** On a start without U-Boot the panel comes up from
  BOOT's screen DTB, and USB 5 V from BOOT's DTB.

## 7. Linux handoff (LinuxBootLib)

This is the `booti` contract: the kernel at a 2 MB aligned address, the
initrd and FDT placed outside every reserved region, x0 = FDT, MMU and
D-cache off. Fixups to the DTB:

- `/memory`: two banks around OP-TEE, from the RAM size. Without a RAM record
  it uses the RAM top the UEFI's own memory map settled on, so Linux never
  starts without `/memory`;
- `/chosen/bootargs` and the initrd range;
- the logo handover: the `drm-logo` reserved region and the route's `logo,*`
  properties when U-Boot's screen is adopted.

`GetMemoryMap` is retried until the buffer fits.

## 8. Building

    git submodule update --init --recursive
    pip install -r pip-requirements.txt   # in a venv, see Mu-Silicium's docs
    VENV=<venv> andr36oid/build.sh r36s DEBUG     # or RELEASE
    VENV=<venv> andr36oid/build.sh qemuvirt DEBUG # QEMU test device

Clang/LLD 19 and iasl are needed. Output: `Mu-r36s.bin`. For r36s the script
also runs `check-image.py`, `check-depex.py` and `emulate-shim.py`.
`Platforms/GameConsole/r36sPkg/Tools/pack-uboot-img.sh` packs the image as a
`uboot.img` (loaderimage format, 4 copies) for M3.

## 9. Testing without hardware

- `make -C Silicon/Rockchip/Tests/host DTB=<an R36S Android DTB>` runs the
  silicon and board suites:
  - clocks, CRU, I2C, SARADC, keys, PMIC, power check, USB, DW-MMC, display;
  - memory map, board facts, PEI, log, reset, SD pins.

  Where a U-Boot or kernel rule exists, the expected values come from that
  code (`uboot_ref_mem.c` holds U-Boot's ATAG, OP-TEE and DRAM code verbatim),
  not from the implementation. `KNOWN()` marks an open bug without failing;
  `STRICT_KNOWN=1` makes those fail.
- `tools/host-test/run.sh`: BootPolicyLib against `boot.ini` (102 cases) and
  the Linux handoff (49).
- `tools/qemu-test.sh` with `REAL_DTB=<R36S DTB>`: the qemuvirt image boots a
  test kernel through the boot app. 11 cases, 82 checks: Android, recovery,
  the menu, the eMMC test, a USB stick, a warm restart with the previous log,
  and Kernel Tweaks.
- Static checks run during development: the clang analyzer, stricter warnings,
  a format-string checker for edk2's `Print`, and ASan/UBSan on the host tests.
- `tools/px30-emu/`: a QEMU machine shaped like the PX30 that runs the real r36s
  image. It has the memory map, a CRU, the VOP and a DW-MMC SD/eMMC model with
  andr36oid card images, plus a small PX30 test kernel.
  - `qemu-build.sh` builds the patched QEMU;
  - `mkcard.sh` makes card images;
  - `run.sh` runs one scenario;
  - `sd-scenarios.sh` runs the SD/eMMC set: IDMAC and PIO, 512 MB, recovery,
    eMMC, starts without U-Boot, no card, and injected CRC errors and
    timeouts.

  Paths default to the developer's machine; set `EMU_DIR` (and `QEMU`, `CARDS`,
  `HOOK`) to override them.

## 10. Testing on hardware

`andr36oid/testkit/` holds the chainload test. `install-uefi-test.sh` adds a
hook to `boot.ini`, copies the firmware as `uefi.bin` and writes `uefi.txt`.
With `uefi=once`, U-Boot marks it `tried` first, so a hang costs one power
cycle and the next start is normal Android. `SMOKE-TEST.md` lists what to look
for in `BOOT/uefi-log.txt`.

## 11. Bugs found while checking the port

The ones that would have broken a start:

- the BootShim crashing every start (literal pool, self-overwrite);
- the SD/eMMC and USB host drivers never dispatched (IOMMU depex);
- the DW-MMC clocks set for the wrong SoC, and card detect inverted;
- 14 MB of OP-TEE handed to Linux (16 MB default instead of 30 MB; no legacy
  block, no flags check);
- the D-28S USB port stuck in charge mode;
- the D-28S boost set to 4.7 V;
- the PMIC's saved rail state overwritten on every chainload start;
- reboot reasons that U-Boot clears showing the charge screen;
- a VOP read with the VO power domain off;
- an eMMC install's data card chosen over the eMMC;
- the SD/eMMC driver re-identifying a slot that failed (an eMMC slot with no
  chip, a dead card) every 100 ms at callback priority. That flooded the log
  and starved the boot menu. It now stops after three tries until the card is
  taken out;
- a changed serial number on starts without U-Boot.

## 12. Open items

- **Hardware test M1.** Not run yet.
- **Starts without U-Boot:**
  - the USB-stick fallback has no 5 V when there is no BOOT with a DTB. This
    is deliberate: without the board DTB the board is unknown, and switching
    5 V blind could fight a charger on boards that share the port;
  - the DC jack pin is GPIO0_B3 only (EE clones use GPIO3_A5, Soy Sauce
    GPIO3_C7);
  - no full boot-on rail pass yet (it would also set `vccio_sd` while the
    card is in use);
  - `rockusb` on "reboot loader" is not replicated.
- **The A/B key pins.** On boards outside the rg351v family they double as eMMC
  data lines 2 and 5 (GPIO1_A2/A5). Setting them to GPIO on an eMMC console may
  break the eMMC. Before changing anything, check on hardware what the iomux
  holds when the app starts.
- **Logo after display reinit.** With `display=reinit` the DT gets the
  `drm-logo` region but not the route's `logo,*`/`video,*` properties. The
  kernel cannot adopt the screen and keeps the region reserved.

## 13. Commit history

`git log --oneline main..` lists every change, with its reason in the subject
line. The full build and test run (`andr36oid/build.sh` for r36s RELEASE and
DEBUG and for qemuvirt, the host tests, the boot app tests and the QEMU suite)
passed on the branch head.
