/** @file
  Host-mode FIFO layout from what the core was built with, as a pure function
  so it can be tested on the host.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef DW_USB_FIFO_H_
#define DW_USB_FIFO_H_

// What the Linux dwc2 driver asks for on Rockchip (params.c dwc2_set_rk_params),
// in 32-bit words.
#define DW_FIFO_WANT_RX      525
#define DW_FIFO_WANT_NPTX    128
#define DW_FIFO_WANT_PTX     256

// Least that still moves a 512-byte bulk packet (RX) and a 64-byte full-speed
// one (TX), plus status words.
#define DW_FIFO_MIN_RX       160
#define DW_FIFO_MIN_NPTX     32
#define DW_FIFO_MIN_PTX      32

typedef struct {
  UINT32    RxDepth;
  UINT32    NpTxStart;
  UINT32    NpTxDepth;
  UINT32    PTxStart;
  UINT32    PTxDepth;
} DW_FIFO_LAYOUT;

/**
  Split the data FIFO RAM (GHWCFG3[31:16] words) into receive, non-periodic and
  periodic transmit FIFOs: the wanted sizes if they fit, else the transmit
  ones shrink toward their minimum and the receive FIFO gets the rest.

  @retval TRUE   Layout filled in.
  @retval FALSE  Even the minimum does not fit.
**/
STATIC
inline
BOOLEAN
DwUsbSizeFifos (
  IN  UINT32          TotalWords,
  OUT DW_FIFO_LAYOUT  *Layout
  )
{
  UINT32  Rx   = DW_FIFO_WANT_RX;
  UINT32  Np   = DW_FIFO_WANT_NPTX;
  UINT32  P    = DW_FIFO_WANT_PTX;
  UINT32  Over;
  UINT32  Cut;

  if (TotalWords < DW_FIFO_MIN_RX + DW_FIFO_MIN_NPTX + DW_FIFO_MIN_PTX) {
    return FALSE;
  }

  if (Rx + Np + P > TotalWords) {
    Over = Rx + Np + P - TotalWords;

    // Periodic first (keyboards and mice need 8 bytes), then non-periodic.
    Cut = P - DW_FIFO_MIN_PTX;
    if (Cut > Over) {
      Cut = Over;
    }

    P    -= Cut;
    Over -= Cut;

    Cut = Np - DW_FIFO_MIN_NPTX;
    if (Cut > Over) {
      Cut = Over;
    }

    Np   -= Cut;
    Over -= Cut;

    // Still too big: the receive FIFO gives the rest back.
    Rx -= Over;
  }

  Layout->RxDepth   = Rx;
  Layout->NpTxStart = Rx;
  Layout->NpTxDepth = Np;
  Layout->PTxStart  = Rx + Np;
  Layout->PTxDepth  = P;
  return TRUE;
}

#endif
