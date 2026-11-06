/** @file
 *
 *  Copyright (c) 2018, Andrey Warkentin <andrey.warkentin@gmail.com>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  andr36oid: the RPi firmware power call became the PX30 platform library
 *  (RkUsbPlatformLib), the controller handle carries a marker protocol the
 *  boot manager uses to connect it at every start.
 *
 **/

#include "DwUsbHostDxe.h"
#include <Guid/RkPlatformInfoHob.h>

STATIC
EFI_STATUS
EFIAPI
DriverSupported (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  );

STATIC
EFI_STATUS
EFIAPI
DriverStart (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  );

STATIC
EFI_STATUS
EFIAPI
DriverStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  UINTN                       NumberOfChildren,
  IN  EFI_HANDLE                  *ChildHandleBuffer
  );

EFI_DRIVER_BINDING_PROTOCOL mDriverBinding = {
  DriverSupported,
  DriverStart,
  DriverStop,
  0xa,
  NULL,
  NULL
};

STATIC EFI_DW_DEVICE_PATH mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
        (UINT8)(sizeof (VENDOR_DEVICE_PATH) + sizeof (UINT64)),
        (UINT8)((sizeof (VENDOR_DEVICE_PATH) + sizeof (UINT64)) >> 8),
      }
    },
    EFI_CALLER_ID_GUID
  },
  0xFF300000,
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    {
      sizeof (EFI_DEVICE_PATH_PROTOCOL),
      0
    }
  }
};

STATIC EFI_HANDLE mDevice;

/**
  The board device tree U-Boot passed us, NULL when there is none (a start
  without U-Boot gets one from the boot app later).
**/
STATIC
CONST VOID *
BootloaderFdt (
  VOID
  )
{
  EFI_HOB_GUID_TYPE           *Hob;
  CONST RK_PLATFORM_INFO_HOB  *Info;

  Hob = GetFirstGuidHob (&gRkPlatformInfoHobGuid);
  if (Hob == NULL) {
    return NULL;
  }

  Info = GET_GUID_HOB_DATA (Hob);
  if ((Info->BootloaderFdt == 0) || (FdtCheckHeader ((VOID *)(UINTN)Info->BootloaderFdt) != 0)) {
    return NULL;
  }

  return (CONST VOID *)(UINTN)Info->BootloaderFdt;
}

STATIC
EFI_STATUS
EFIAPI
DriverSupported (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  )
{
  VOID *Temp;

  if (Controller != mDevice) {
    return EFI_UNSUPPORTED;
  }

  if (gBS->HandleProtocol (Controller, &gEfiUsb2HcProtocolGuid,
             (VOID**)&Temp) == EFI_SUCCESS) {
    return EFI_ALREADY_STARTED;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DriverStart (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  )
{
  VOID *Dummy;
  EFI_STATUS Status;
  DWUSB_OTGHC_DEV *DwHc = NULL;
  UINT64 Base;
  UINT64 Start;

  Start = GetPerformanceCounter ();

  Status = gBS->OpenProtocol (
                  Controller,
                  &gRkUsbHostControllerGuid,
                  (VOID**)&Dummy,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = RkUsbPlatformPowerOn (&Base);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DwUsbHost: USB power on failed: %r\n", Status));
    goto Exit;
  }

  Status = RkUsbPlatformApplyFdt (BootloaderFdt ());
  if (Status == EFI_NOT_READY) {
    // Charger in on a shared port: no 5 V for devices, the stack still comes up.
    DEBUG ((DEBUG_INFO, "DwUsbHost: port left in charge mode\n"));
  }

  Status = CreateDwUsbHc (Base, &DwHc);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  /*
   * UsbBusDxe as of b4e96b82b4e2e47e95014b51787ba5b43abac784 expects
   * the HCD to do this. There is no agent invoking DwHcReset anymore.
   */
  Status = DwHcReset (&DwHc->DwUsbOtgHc, EFI_USB_HC_RESET_HOST_CONTROLLER);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }

  DwHcSetState (&DwHc->DwUsbOtgHc, EfiUsbHcStateOperational);

  Status = gBS->InstallMultipleProtocolInterfaces (
    &Controller,
    &gEfiUsb2HcProtocolGuid, &DwHc->DwUsbOtgHc,
    NULL
  );

Exit:
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Could not start DwUsbHostDxe: %r\n", Status));
    DestroyDwUsbHc (DwHc);
    gBS->CloseProtocol (
           Controller,
           &gRkUsbHostControllerGuid,
           This->DriverBindingHandle,
           Controller
         );
  } else {
    DEBUG ((DEBUG_INFO, "DwUsbHost: started in %lu ms\n",
      DivU64x32 (GetTimeInNanoSecond (GetPerformanceCounter () - Start), 1000000)));
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
DriverStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL *This,
  IN  EFI_HANDLE                  Controller,
  IN  UINTN                       NumberOfChildren,
  IN  EFI_HANDLE                  *ChildHandleBuffer
  )
{
  EFI_STATUS Status;
  DWUSB_OTGHC_DEV *DwHc;
  EFI_USB2_HC_PROTOCOL *HcProtocol;

  Status = gBS->HandleProtocol (
                  Controller,
                  &gEfiUsb2HcProtocolGuid,
                  (VOID**)&HcProtocol
                );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DriverStop: HandleProtocol: %r\n", Status));
    return Status;
  }

  DwHc = DWHC_FROM_THIS (HcProtocol);

  Status = gBS->UninstallMultipleProtocolInterfaces (
    Controller,
    &gEfiUsb2HcProtocolGuid, &DwHc->DwUsbOtgHc,
    NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "DriverStop: UninstallMultipleProtocolInterfaces: %r\n",
      Status));
    return Status;
  }

  DwHcQuiesce (DwHc);
  DestroyDwUsbHc (DwHc);

  gBS->CloseProtocol (
         Controller,
         &gRkUsbHostControllerGuid,
         This->DriverBindingHandle,
         Controller
       );

  return EFI_SUCCESS;
}

/**
   UEFI Driver Entry Point API

   @param  ImageHandle       EFI_HANDLE.
   @param  SystemTable       EFI_SYSTEM_TABLE.

   @return EFI_SUCCESS       Success.
   EFI_DEVICE_ERROR  Fail.
**/

EFI_STATUS
EFIAPI
DwUsbHostEntryPoint (
  IN  EFI_HANDLE ImageHandle,
  IN  EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  Status = gBS->InstallMultipleProtocolInterfaces (
    &mDevice,
    &gEfiDevicePathProtocolGuid, &mDevicePath,
    &gRkUsbHostControllerGuid, NULL,
    NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "InstallMultipleProtocolInterfaces: %r\n", Status));
    return Status;
  }

  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &mDriverBinding,
             ImageHandle,
             &gComponentName,
             &gComponentName2
           );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "EfiLibInstallDriverBindingComponentName2: %r\n", Status));
    gBS->UninstallMultipleProtocolInterfaces (
           mDevice,
           &gEfiDevicePathProtocolGuid, &mDevicePath,
           &gRkUsbHostControllerGuid, NULL,
           NULL
         );
  }

  return Status;
}
