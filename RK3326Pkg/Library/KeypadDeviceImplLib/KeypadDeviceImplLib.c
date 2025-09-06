#include <PiDxe.h>

#include <Library/KeypadDeviceImplLib.h>
#include <Library/KeypadDeviceHelperLib.h>
#include <Protocol/KeypadDevice.h>
#include <Library/GpioLib.h>

typedef enum {
  KEY_DEVICE_TYPE_UNKNOWN,

  KEY_DEVICE_TYPE_GPIO,
  KEY_DEVICE_TYPE_PMIC,
} KEY_DEVICE_TYPE;

typedef struct {
  KEY_CONTEXT      EfiKeyContext;
  KEY_DEVICE_TYPE  DeviceType;

  // gpio
  UINT8            GpioBank;
  UINT8            Gpio;
  BOOLEAN          ActiveLow;
} KEY_CONTEXT_PRIVATE;

STATIC KEY_CONTEXT_PRIVATE KeyContextDpadUp;
STATIC KEY_CONTEXT_PRIVATE KeyContextDpadDown;
STATIC KEY_CONTEXT_PRIVATE KeyContextDpadLeft;
STATIC KEY_CONTEXT_PRIVATE KeyContextDpadRight;
STATIC KEY_CONTEXT_PRIVATE KeyContextBtnA;
STATIC KEY_CONTEXT_PRIVATE KeyContextBtnB;
STATIC KEY_CONTEXT_PRIVATE KeyContextBtnX;
STATIC KEY_CONTEXT_PRIVATE KeyContextBtnY;

STATIC KEY_CONTEXT_PRIVATE* KeyList[] = {
  &KeyContextDpadUp,
  &KeyContextDpadDown,
  &KeyContextDpadLeft,
  &KeyContextDpadRight,
  &KeyContextBtnA,
  &KeyContextBtnB,
  &KeyContextBtnX,
  &KeyContextBtnY
};

STATIC
VOID
KeypadInitializeKeyContextPrivate (
  KEY_CONTEXT_PRIVATE  *Context
  )
{
  Context->GpioBank = 0;
  Context->Gpio = 0;
  Context->DeviceType = KEY_DEVICE_TYPE_UNKNOWN;
  Context->ActiveLow = FALSE;
}

STATIC
KEY_CONTEXT_PRIVATE*
KeypadKeyCodeToKeyContext (
  UINT32 KeyCode
  )
{
  if (KeyCode == 544){
    return &KeyContextDpadUp;
  }else if (KeyCode == 545){
    return &KeyContextDpadDown;
  }else if (KeyCode == 546){
    return &KeyContextDpadLeft;
  }else if (KeyCode == 547){
    return &KeyContextDpadRight;
  }else if (KeyCode == 304){
    return &KeyContextBtnA;
  }else if (KeyCode == 305){
    return &KeyContextBtnB;
  }else if (KeyCode == 307){
    return &KeyContextBtnX;
  }else if (KeyCode == 308){
    return &KeyContextBtnY;
  }else
    return NULL;
}

RETURN_STATUS
EFIAPI
KeypadDeviceImplConstructor (
  VOID
  )
{
  UINTN                          Index;
  KEY_CONTEXT_PRIVATE            *StaticContext;

  // Reset all keys
  for (Index=0; Index<ARRAY_SIZE(KeyList); Index++) {
    KeypadInitializeKeyContextPrivate (KeyList[Index]);
  }

  // Configure keys

  // Dpad Up
  StaticContext = KeypadKeyCodeToKeyContext(544);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_GPIO;
  StaticContext->GpioBank = 1;
  StaticContext->Gpio = GPIO_PIN_PB4;
  StaticContext->ActiveLow = TRUE;

  //Dpad Down
  StaticContext = KeypadKeyCodeToKeyContext(545);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_GPIO;
  StaticContext->GpioBank = 1;
  StaticContext->Gpio = GPIO_PIN_PB5;
  StaticContext->ActiveLow = TRUE;

  //Dpad Left
  StaticContext = KeypadKeyCodeToKeyContext(546);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_GPIO;
  StaticContext->GpioBank = 1;
  StaticContext->Gpio = GPIO_PIN_PB6;
  StaticContext->ActiveLow = TRUE;
  //Dpad Right
  StaticContext = KeypadKeyCodeToKeyContext(547);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_GPIO;
  StaticContext->GpioBank = 1;
  StaticContext->Gpio = GPIO_PIN_PB7;
  StaticContext->ActiveLow = TRUE;

  //A Button
  StaticContext = KeypadKeyCodeToKeyContext(304);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_GPIO;
  StaticContext->GpioBank = 1;
  StaticContext->Gpio = GPIO_PIN_PA2;
  StaticContext->ActiveLow = TRUE;

  //B Button
  StaticContext = KeypadKeyCodeToKeyContext(305);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_GPIO;
  StaticContext->GpioBank = 1;
  StaticContext->Gpio = GPIO_PIN_PA5;
  StaticContext->ActiveLow = TRUE;

  //Y button
  StaticContext = KeypadKeyCodeToKeyContext(307);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_GPIO;
  StaticContext->GpioBank = 1;
  StaticContext->Gpio = GPIO_PIN_PA6;
  StaticContext->ActiveLow = TRUE;

  //X button
  StaticContext = KeypadKeyCodeToKeyContext(308);
  StaticContext->DeviceType = KEY_DEVICE_TYPE_GPIO;
  StaticContext->GpioBank = 1;
  StaticContext->Gpio = GPIO_PIN_PA7;
  StaticContext->ActiveLow = TRUE;


  return RETURN_SUCCESS;
}

EFI_STATUS EFIAPI KeypadDeviceImplReset (KEYPAD_DEVICE_PROTOCOL *This)
{
  LibKeyInitializeKeyContext(&KeyContextBtnA.EfiKeyContext);
  KeyContextBtnA.EfiKeyContext.KeyData.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;

  LibKeyInitializeKeyContext(&KeyContextBtnB.EfiKeyContext);
  KeyContextBtnB.EfiKeyContext.KeyData.Key.ScanCode = SCAN_ESC;

  LibKeyInitializeKeyContext(&KeyContextBtnY.EfiKeyContext);
  KeyContextBtnY.EfiKeyContext.KeyData.Key.ScanCode = SCAN_F2;

  LibKeyInitializeKeyContext(&KeyContextDpadUp.EfiKeyContext);
  KeyContextDpadUp.EfiKeyContext.KeyData.Key.ScanCode = SCAN_UP;

  LibKeyInitializeKeyContext(&KeyContextDpadDown.EfiKeyContext);
  KeyContextDpadDown.EfiKeyContext.KeyData.Key.ScanCode = SCAN_DOWN;

  LibKeyInitializeKeyContext(&KeyContextDpadLeft.EfiKeyContext);
  KeyContextDpadLeft.EfiKeyContext.KeyData.Key.ScanCode = SCAN_LEFT;

  LibKeyInitializeKeyContext(&KeyContextDpadRight.EfiKeyContext);
  KeyContextDpadRight.EfiKeyContext.KeyData.Key.ScanCode = SCAN_RIGHT;

  return EFI_SUCCESS;
}

EFI_STATUS KeypadDeviceImplGetKeys (KEYPAD_DEVICE_PROTOCOL *This, KEYPAD_RETURN_API *KeypadReturnApi, UINT64 Delta)
{
  BOOLEAN  IsPressed;
  UINTN    Index;

    for (Index = 0; Index < (sizeof(KeyList) / sizeof(KeyList[0])); Index++) {
        KEY_CONTEXT_PRIVATE *Context = KeyList[Index];
      

    IsPressed = FALSE;
    // get status
    if(GpioPinRead(Context->GpioBank, Context->Gpio) == 1)
    {
      IsPressed = TRUE;
      DEBUG((EFI_D_INFO | EFI_D_LOAD, "IsPressed= %d\n", IsPressed));
    }
    //DEBUG((EFI_D_INFO | EFI_D_LOAD, "GpioBank:%u, Gpio:%u, GpioStatus:%u\n", Context->GpioBank, Context->Gpio, GpioStatus));
    //DEBUG((EFI_D_INFO | EFI_D_LOAD, "IsPressed is:%u\n", IsPressed));
    LibKeyUpdateKeyStatus(&Context->EfiKeyContext, KeypadReturnApi, IsPressed, Delta);
  }

  return EFI_SUCCESS;
}
