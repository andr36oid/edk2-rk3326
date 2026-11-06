/** @file
  RK3326 board facts readable from SEC: RAM size, live framebuffer.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/IoLib.h>
#include <Library/RkPlatformInfoLib.h>
#include <Soc/Rk3326.h>

//
// The DDR init records the geometry in PMUGRF OS_REG2/OS_REG3, the same
// encoding Rockchip's U-Boot decodes in rockchip_sdram_size().
//
#define BITS(v, s, m)  (((v) >> (s)) & (m))

UINT64
EFIAPI
RkGetDramSize (
  VOID
  )
{
  UINT32  Reg  = MmioRead32 (RK3326_PMUGRF_OS_REG (2));
  UINT32  Reg1 = MmioRead32 (RK3326_PMUGRF_OS_REG (3));
  UINT32  Channels = 1 + BITS (Reg, 12, 1);
  UINT32  DramType = BITS (Reg, 13, 7);
  UINT64  TotalMb  = 0;
  UINT32  Ch;

  for (Ch = 0; Ch < Channels; Ch++) {
    UINT32  Rank   = 1 + BITS (Reg, 11 + Ch * 16, 1);
    UINT32  Col0   = 9 + BITS (Reg, 9 + Ch * 16, 3);
    UINT32  Col1   = Col0;
    UINT32  Bank   = 3 - BITS (Reg, 8 + Ch * 16, 1);
    UINT32  Bw     = 2 >> BITS (Reg, 2 + Ch * 16, 3);
    UINT32  Row34  = BITS (Reg, 30 + Ch, 1);
    UINT32  Bg     = 0;
    UINT32  Row0, Row1;
    UINT64  ChipMb;

    if (BITS (Reg1, 28, 0xF) == 2) {
      UINT32  Ext0 = BITS (Reg1, 5 + Ch * 2, 1) << 2;
      UINT32  Ext1 = BITS (Reg1, 4 + Ch * 2, 1) << 2;

      Col1 = 9 + BITS (Reg1, Ch * 2, 3);
      Row0 = (Ext0 + BITS (Reg, 6 + Ch * 16, 3) == 7) ? 12 : 13 + BITS (Reg, 6 + Ch * 16, 3) + Ext0;
      Row1 = (Ext1 + BITS (Reg, 4 + Ch * 16, 3) == 7) ? 12 : 13 + BITS (Reg, 4 + Ch * 16, 3) + Ext1;
    } else {
      Row0 = 13 + BITS (Reg, 6 + Ch * 16, 3);
      Row1 = 13 + BITS (Reg, 4 + Ch * 16, 3);
    }

    if (DramType == 0 /* DDR4 */) {
      // Only DDR4 uses bank groups; PX30 boards here are DDR3/LPDDR3, but
      // keep the decode honest.
      Bg = (BITS (Reg, Ch * 16, 3) == 2) ? 2 : 1;
    }

    if (Row0 + Col0 + Bank + Bg + Bw < 20 || Row0 + Col0 + Bank + Bg + Bw > 34) {
      return 0;
    }

    ChipMb = 1ULL << (Row0 + Col0 + Bank + Bg + Bw - 20);
    if (Rank > 1) {
      ChipMb += ChipMb >> ((Row0 - Row1) + (Col0 - Col1));
    }

    if (Row34) {
      ChipMb = ChipMb * 3 / 4;
    }

    TotalMb += ChipMb;
  }

  // PX30 addresses at most 4 GB; anything outside 128 MB..4 GB is a bad record.
  if (TotalMb < 128 || TotalMb > 4096) {
    return 0;
  }

  return TotalMb << 20;
}

STATIC
BOOLEAN
ReadVopWindow (
  IN  UINTN                VopBase,
  OUT RK_FRAMEBUFFER_INFO  *Info
  )
{
  UINT32  Ctrl = MmioRead32 (VopBase + VOP_WIN1_CTRL0);
  UINT32  HAct = MmioRead32 (VopBase + VOP_DSP_HACT_ST_END);
  UINT32  VAct = MmioRead32 (VopBase + VOP_DSP_VACT_ST_END);
  UINT32  Info1 = MmioRead32 (VopBase + VOP_WIN1_DSP_INFO);
  UINT32  Vir  = MmioRead32 (VopBase + VOP_WIN1_VIR) & 0x1FFF;
  UINT32  Mst  = MmioRead32 (VopBase + VOP_WIN1_MST);
  UINT32  Width, Height;

  if ((MmioRead32 (VopBase + VOP_SYS_CTRL2) & VOP_STANDBY) != 0 ||
      (Ctrl & VOP_WIN_EN) == 0 || Mst == 0 || Vir == 0)
  {
    return FALSE;
  }

  switch (VOP_WIN_FMT (Ctrl)) {
    case VOP_FMT_ARGB8888: Info->BitsPerPixel = 32; break;
    case VOP_FMT_RGB888:   Info->BitsPerPixel = 24; break;
    case VOP_FMT_RGB565:   Info->BitsPerPixel = 16; break;
    default:               return FALSE;
  }

  // The window size; the display timing is the fallback.
  Width  = (Info1 & 0x1FFF) + 1;
  Height = ((Info1 >> 16) & 0x1FFF) + 1;
  if (Width < 16 || Height < 16) {
    Width  = (HAct & 0x1FFF) - ((HAct >> 16) & 0x1FFF);
    Height = (VAct & 0x1FFF) - ((VAct >> 16) & 0x1FFF);
  }

  if (Width < 16 || Height < 16 || Width > 4096 || Height > 4096) {
    return FALSE;
  }

  Info->Base    = Mst;
  Info->Width   = Width;
  Info->Height  = Height;
  Info->Stride  = Vir * 4;
  Info->VopBase = VopBase;
  return Info->Stride >= Width * (Info->BitsPerPixel / 8);
}

EFI_STATUS
EFIAPI
RkGetActiveFramebuffer (
  OUT RK_FRAMEBUFFER_INFO  *Info
  )
{
  if (ReadVopWindow (RK3326_VOPB_BASE, Info) || ReadVopWindow (RK3326_VOPL_BASE, Info)) {
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}
