/** @file
  XVE pre-pass for the NVIDIA CMP 50HX (TU102, PCI 10de:1e09).

  Copyright (c) 2026, MIT licence, same terms as ReBarUEFI.
  SPDX-License-Identifier: MIT
**/

#ifndef _XVE_TU102_H_
#define _XVE_TU102_H_

/* config space helpers provided by ReBar.c */
EFI_STATUS
pciReadConfigDword (
  UINTN  pciAddress,
  INTN   pos,
  UINT32 *buf
  );

EFI_STATUS
pciWriteConfigDword (
  UINTN  pciAddress,
  INTN   pos,
  UINT32 *buf
  );

EFI_STATUS
pciReadConfigByte (
  UINTN  pciAddress,
  INTN   pos,
  UINT8  *buf
  );

UINT16
pciFindExtCapability (
  UINTN  pciAddress,
  INTN   cap
  );

INTN
pciRebarFindPos (
  UINTN  pciAddress,
  INTN   pos,
  UINT8  bar
  );

INTN
pciRebarSetSize (
  UINTN  pciAddress,
  UINTN  epos,
  UINT8  bar,
  UINT8  size
  );

/**
  Append one line to the non-volatile UEFI variable XvePrepassLog (GUID
  a3c5b77a-c88f-4a93-bf1c-4a92a32c65ce) and to DEBUG().  ReBar.c uses this to
  record that the driver itself ran, independently of the XVE pre-pass.

  @param[in] Line  NUL terminated string, no CR/LF required.
**/
VOID
XveLogLine (
  IN CONST CHAR16  *Line
  );

/**
  Write the XVE registers that unlock a large BAR1 on the CMP 50HX (10de:1e09)
  and program the PCIe Resizable BAR control register of BAR1, before the
  firmware hands the BAR sizes to the devices.

  Called from the PreprocessController hook for every enumerated PCI function;
  returns immediately for anything that is not a 10de:1e09 device and for
  devices that were already handled during this boot.

  The BAR1 selector comes from the XveBar1Selector variable (1..9, size =
  2^(selector+6) MiB, 0 disables the pre-pass); without that variable the safe
  bring-up default selector 1 (128 MiB) is used.  Flashing a new image resets
  the variable store, so the default has to work without any variable.

  @param[in] PciAddress   EFI_PCI_ADDRESS() encoded function address.
  @param[in] VendorId     PCI vendor id of the device.
  @param[in] DeviceId     PCI device id of the device.
  @param[in] ReBarState   ReBarUEFI state from the ReBarState variable; it only
                          affects the generic resize pass, the pre-pass runs
                          regardless (kept for logging/diagnostics).
**/
VOID
XveTu102Prepass (
  IN UINTN   PciAddress,
  IN UINT16  VendorId,
  IN UINT16  DeviceId,
  IN UINT8   ReBarState
  );

#endif
