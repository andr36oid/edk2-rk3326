/** @file
  GOP (one fixed mode) on a framebuffer some VOP is already scanning out.
  RkFbDxe uses it for U-Boot's screen, RkDisplayDxe for the one it set up.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef RK_GOP_LIB_H_
#define RK_GOP_LIB_H_

#include <Library/RkPlatformInfoLib.h>

/**
  Install EFI_GRAPHICS_OUTPUT_PROTOCOL and a vendor device path for Fb on a
  new handle. 32 bpp is ARGB8888 (B, G, R, A in memory), 24 and 16 bpp are
  RGB888 and RGB565 as the VOP reads them. Only one GOP per image.
**/
EFI_STATUS
EFIAPI
RkGopInstall (
  IN  CONST RK_FRAMEBUFFER_INFO  *Fb,
  OUT EFI_HANDLE                 *Handle OPTIONAL
  );

#endif
