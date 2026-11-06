/** @file
  The firmware log: everything DEBUG() printed this boot, kept in a reserved
  RAM ring (these consoles have no serial port wired out). The ring of the
  previous boot is kept too: RAM survives a quick reset, so after a hang the
  next start can still save what the hung one printed.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _RK_LOG_LIB_H_
#define _RK_LOG_LIB_H_

#include <Uefi.h>

#define RK_LOG_BASE   0x01E00000      // reserved in the memory map ("UEFI Log")
#define RK_LOG_SIZE   0x00100000      // current half + previous-boot half

/**
  Start this boot's log: the old one (if any) becomes the previous-boot log.
  Called once, early in SEC.
**/
VOID
EFIAPI
RkLogStart (
  VOID
  );

/**
  Copy out a log, oldest byte first. Previous = TRUE for the previous boot's.
  Returns the number of bytes copied (0 if there is none).
**/
UINTN
EFIAPI
RkLogRead (
  IN  BOOLEAN  Previous,
  OUT CHAR8    *Buffer,
  IN  UINTN    BufferSize
  );

/**
  Stop (or resume) drawing the log on the screen, e.g. once a menu or the
  graphical console owns it. The log is still kept in RAM.
**/
VOID
EFIAPI
RkLogScreen (
  IN BOOLEAN  Enable
  );

/**
  Draw the log on a framebuffer the display driver brought up (none at SEC on
  a start without U-Boot, or a new one after a re-init). The newest part of
  the log is drawn again so what happened before the panel came up shows.
  Only 32 and 16 bpp are drawn; anything else turns the screen log off.
**/
VOID
EFIAPI
RkLogSetFramebuffer (
  IN UINT64  Base,
  IN UINT32  Width,
  IN UINT32  Height,
  IN UINT32  Stride,
  IN UINT32  BitsPerPixel
  );

/**
  Mark the log as handed to Linux (TRUE) right before the kernel starts, or
  take the mark back (FALSE) if starting it failed. Linux reuses this RAM, so
  the next start keeps no previous copy of a handed-off log: the previous log
  is only kept for a start that died inside the firmware.
**/
VOID
EFIAPI
RkLogHandoff (
  IN BOOLEAN  HandedOff
  );

/**
  Bytes a full RkLogRead would return.
**/
UINTN
EFIAPI
RkLogSize (
  IN BOOLEAN  Previous
  );

#endif
