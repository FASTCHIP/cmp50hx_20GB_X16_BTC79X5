/** @file
  XVE pre-pass for the NVIDIA CMP 50HX (TU102, PCI 10de:1e09) used on the
  BTC79X5 / X79ETH03 boards.

  Background
  ----------
  On these boards the maximum BAR1 size of the CMP 50HX is clamped inside the
  GPU's XVE (BAR/external-view control) block and is not advertised in the PCIe
  Resizable BAR capability: only 64 MiB is reported.  The known fix (CMP50-unlock
  package, patch 03-cmp50-rebar.patch, applied on the Linux side) writes:

    BAR0 + 0x88724 (CYA unlock)      <- 0x00000030
    BAR0 + 0x88DCC (XVE ReBAR cfg)   <- bit31 | selector      (selector 8 = 16 GiB)
    BAR0 + 0x88BBC (supported sizes) -> read back, grows 0x400 -> 0x7FC00

  BAR1 size = 2^(selector + 6) MiB, i.e. selector 1..8 covers 128 MiB..16 GiB and
  selector 9 would be 32 GiB.

  Writing it from the operating system is too late: by then the firmware has
  already assigned BAR1 (64 MiB) and, if a large BAR1 is forced afterwards, the
  NVIDIA driver fails its BAR2 self test (kbusVerifyBar2 -> RmInitAdapter
  failed 0x24:0x72:1281).

  What this module does
  ---------------------
  ReBarDxe hooks EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL.PreprocessController
  and resizes the PCIe Resizable BAR registers of every device during PCI
  enumeration, i.e. before EDK2's PciBusDxe reads the BAR sizes (PciSearchDevice /
  PciParseBar) and long before the firmware programs the BARs.

  This file adds a pre-pass that runs from the same hook, for 10de:1e09 devices
  only, immediately *before* the ReBAR resize pass of ReBarDxe.  It writes the two
  XVE registers described above, so that when ReBarDxe (and the firmware) later
  look at the device, the hardware already offers a 16 GiB BAR1.

  BAR0 access during enumeration
  ------------------------------
  At EfiPciBeforeResourceCollection time the device BARs are not programmed yet,
  so in general there is no address at which BAR0 can be reached.  The pre-pass
  therefore works in two steps:

    1. if config space already holds a decoded BAR0 address, use it directly;
    2. otherwise temporarily program BAR0 to a scratch window taken from the
       PCI memory-mapped I/O aperture (allocated through gDS, top-down), open the
       memory windows of the upstream bridges so that the scratch address is
       routed to the device, do the XVE writes, and restore every register.

  Both paths verify the result by reading back the XVE registers: the XVE
  capability register at +0x88BBC must look like a ReBAR size bitmap (this is
  used as a signature check before any write) and, after the writes, the
  capability bitmap must advertise the requested size.  If nothing is decoded
  the reads return 0xFFFF_FFFF and the pre-pass is reported as failed without
  having written anything but the (restored) BAR0 value.

  Diagnostics
  -----------
  Every attempt is logged through DEBUG() and appended to the non-volatile UEFI
  variable XvePrepassLog (GUID a3c5b77a-c88f-4a93-bf1c-4a92a32c65ce, the same
  GUID as ReBarState).  After a boot cycle the log can be read from Linux with

    printf '%s\n' "$(cat /sys/firmware/efi/efivars/XvePrepassLog-a3c5b77a-c88f-4a93-bf1c-4a92a32c65ce | tail -c +5 | tr -d '\0')"

  The BAR1 selector can be overridden without reflashing by setting the variable
  XveBar1Selector (same GUID, one byte, 1..9); the default is 8 (16 GiB).

  Copyright (c) 2026, MIT licence, same terms as ReBarUEFI.
  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Protocol/PciRootBridgeIo.h>

#include "include/pciRegs.h"
#include "include/XveTu102.h"

#define XVE_VENDOR_ID  0x10DE
#define XVE_DEVICE_ID  0x1E09

/* offsets are relative to BAR0, values taken from the CMP50-unlock reference */
#define XVE_CYA_OFFSET  0x88724U
#define XVE_CAP_OFFSET  0x88BBCU
#define XVE_CFG_OFFSET  0x88DCCU

#define XVE_CYA_UNLOCK_VALUE  0x00000030U
#define XVE_CFG_ENABLE        BIT31
#define XVE_CFG_SIZE_MASK     0x0000000FU

/* XVE capability register signature: 64 MiB (bit 10) is always advertised and
   bits 0..9 are reserved, so a stale or foreign mapping can be told apart */
#define XVE_CAP_MUST_BIT      BIT10
#define XVE_CAP_RESERVED_MASK 0x000003FFU

#define XVE_BAR0_SIZE         0x01000000ULL  /* 16 MiB */
#define XVE_BAR0_ALIGN_SHIFT  24U            /* 2^24 */

#define XVE_SELECTOR_DEFAULT  1U             /* 128 MiB - safe bring-up default */
#define XVE_SELECTOR_TARGET   8U             /* 16 GiB - selected via the variable */
#define XVE_SELECTOR_MAX      9U             /* 32 GiB, untested on this board */
#define XVE_SELECTOR_OFF      0U

#define XVE_SCRATCH_FALLBACK  0xFB000000ULL  /* top of the 32-bit PCI window */

#define XVE_MAX_BRIDGES  6U

#define XVE_LOG_MAX_SIZE  3072U

STATIC EFI_GUID  mXveVariableGuid = {
  0xa3c5b77a, 0xc88f, 0x4a93, { 0xbf, 0x1c, 0x4a, 0x92, 0xa3, 0x2c, 0x65, 0xce }
};

typedef struct {
  UINTN    Address;
  UINT32   Command;
  UINT32   MemBaseLimit;
  UINT32   PrefBaseLimit;
  UINT32   PrefBaseUpper;
  UINT32   PrefLimitUpper;
} XVE_BRIDGE_STATE;

typedef struct {
  UINT32    CyaOld;
  UINT32    CyaNew;
  UINT32    CfgOld;
  UINT32    CfgNew;
  UINT32    CapOld;
  UINT32    CapNew;
  BOOLEAN   SignatureOk;
  BOOLEAN   Verified;
} XVE_RESULT;

STATIC UINTN  mXveHandled[8];
STATIC UINTN  mXveHandledCount = 0;

/* ------------------------------------------------------------------------- */
/* MMIO                                                                      */
/* ------------------------------------------------------------------------- */

STATIC
UINT32
XveMmioRead32 (
  IN UINT64  Base,
  IN UINT32  Offset
  )
{
  UINT32  Value;

  CopyMem (&Value, (VOID *)(UINTN)(Base + Offset), sizeof (Value));
  return Value;
}

STATIC
VOID
XveMmioWrite32 (
  IN UINT64  Base,
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  CopyMem ((VOID *)(UINTN)(Base + Offset), &Value, sizeof (Value));
}

/* ------------------------------------------------------------------------- */
/* logging                                                                   */
/* ------------------------------------------------------------------------- */

VOID
XveLogLine (
  IN CONST CHAR16  *Line
  )
{
  EFI_STATUS  Status;
  CHAR16      *Old;
  CHAR16      *New;
  UINT32      Attributes;
  UINTN       OldSize;
  UINTN       LineChars;
  UINTN       TotalChars;

  DEBUG ((DEBUG_INFO, "ReBarDXE XVE: %s\n", Line));

  OldSize = XVE_LOG_MAX_SIZE;
  Old     = AllocatePool (OldSize);
  if (Old == NULL) {
    return;
  }

  Status = gRT->GetVariable (L"XvePrepassLog", &mXveVariableGuid, &Attributes, &OldSize, Old);
  if (EFI_ERROR (Status)) {
    OldSize = 0;
  }

  LineChars = StrLen (Line);
  if (LineChars == 0) {
    FreePool (Old);
    return;
  }

  if (OldSize / sizeof (CHAR16) + LineChars + 2 > XVE_LOG_MAX_SIZE / sizeof (CHAR16)) {
    OldSize = 0; /* keep the newest lines only */
  }

  TotalChars = OldSize / sizeof (CHAR16);
  New        = AllocatePool ((TotalChars + LineChars + 2) * sizeof (CHAR16));
  if (New == NULL) {
    FreePool (Old);
    return;
  }

  if (TotalChars != 0) {
    CopyMem (New, Old, OldSize);
  }

  CopyMem (New + TotalChars, Line, LineChars * sizeof (CHAR16));
  New[TotalChars + LineChars]     = L'\r';
  New[TotalChars + LineChars + 1] = L'\n';

  Status = gRT->SetVariable (
                  L"XvePrepassLog",
                  &mXveVariableGuid,
                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                  (TotalChars + LineChars + 2) * sizeof (CHAR16),
                  New
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "ReBarDXE XVE: var log write failed %r\n", Status));
  }

  FreePool (New);
  FreePool (Old);
}

STATIC
VOID
XveLogDevice (
  IN CONST CHAR16  *Tag,
  IN UINTN         PciAddress,
  IN UINT8         Selector,
  IN BOOLEAN       FromVariable,
  IN UINT64        Base,
  IN CONST XVE_RESULT *Result
  )
{
  CHAR16  Line[XVE_LOG_MAX_SIZE / sizeof (CHAR16)];

  UnicodeSPrint (
    Line,
    sizeof (Line),
    L"[%s] %02x:%02x.%x sel=%d src=%d base=%08x%08x cya=%08x->%08x cfg=%08x->%08x cap=%08x->%08x sig=%d ok=%d",
    Tag,
    (UINTN)((PciAddress >> 24) & 0xFF),
    (UINTN)((PciAddress >> 16) & 0x1F),
    (UINTN)((PciAddress >> 8) & 0x07),
    (UINTN)Selector,
    (UINTN)(FromVariable ? 1 : 0),
    (UINT32)(Base >> 32),
    (UINT32)(Base & 0xFFFFFFFF),
    Result->CyaOld,
    Result->CyaNew,
    Result->CfgOld,
    Result->CfgNew,
    Result->CapOld,
    Result->CapNew,
    (UINTN)Result->SignatureOk,
    (UINTN)Result->Verified
    );
  XveLogLine (Line);
}

/**
  Log the PCIe Resizable BAR control register of BAR1 (the size the firmware will
  hand to PciBusDxe).

  @param[in] Tag          short marker for the log line.
  @param[in] PciAddress   EFI_PCI_ADDRESS() encoded function address.
  @param[in] Selector     XVE selector, size = 2^(selector+6) MiB.
  @param[in] CapMask      PCIe ReBAR supported sizes bitmap of BAR1 (bit n = 2^(n+20) bytes).
  @param[in] OldSize      BAR1 "BAR Size" field before the pre-pass.
  @param[in] NewSize      BAR1 "BAR Size" field after the pre-pass.
  @param[in] Ok           whether the register now holds the requested size.
**/
STATIC
VOID
XveLogPcie (
  IN CONST CHAR16  *Tag,
  IN UINTN         PciAddress,
  IN UINT8         Selector,
  IN UINT32        CapMask,
  IN UINT8         OldSize,
  IN UINT8         NewSize,
  IN UINT32        CapPos,
  IN BOOLEAN       Ok
  )
{
  CHAR16  Line[XVE_LOG_MAX_SIZE / sizeof (CHAR16)];

  UnicodeSPrint (
    Line,
    sizeof (Line),
    L"[pcie-%s] %02x:%02x.%x sel=%d bin=%d cp=%08x mask=%08x old=%d new=%d ok=%d",
    Tag,
    (UINTN)((PciAddress >> 24) & 0xFF),
    (UINTN)((PciAddress >> 16) & 0x1F),
    (UINTN)((PciAddress >> 8) & 0x07),
    (UINTN)Selector,
    (UINTN)(Selector + 6U),
    CapPos,
    CapMask,
    (UINTN)OldSize,
    (UINTN)NewSize,
    (UINTN)Ok
    );
  XveLogLine (Line);
}

/* ------------------------------------------------------------------------- */
/* selector                                                                  */
/* ------------------------------------------------------------------------- */

/**
  Read the BAR1 selector.

  The pre-pass is active by default (selector 1 = 128 MiB, the smallest step
  above the stock 64 MiB BAR1) because a freshly flashed image cannot rely on any
  UEFI variable: flashing rewrites the variable store.  The target size is
  selected with the non-volatile variable XveBar1Selector (1 byte); 0 disables
  the pre-pass.  Out-of-range values fall back to the safe default.

  @param[out] FromVariable  TRUE when the value came from the variable.
**/
STATIC
UINT8
XveGetSelector (
  OUT BOOLEAN  *FromVariable
  )
{
  EFI_STATUS  Status;
  UINT32      Attributes;
  UINT8       Value;
  UINTN       Size;

  *FromVariable = FALSE;
  Value  = XVE_SELECTOR_DEFAULT;
  Size   = sizeof (Value);
  Status = gRT->GetVariable (L"XveBar1Selector", &mXveVariableGuid, &Attributes, &Size, &Value);
  if (!EFI_ERROR (Status)) {
    *FromVariable = TRUE;
    if (Value > XVE_SELECTOR_MAX) {
      Value = XVE_SELECTOR_DEFAULT;
      *FromVariable = FALSE;
    }
  } else {
    Value = XVE_SELECTOR_DEFAULT;
  }

  return Value;
}

/* ------------------------------------------------------------------------- */
/* PCIe Resizable BAR control register of BAR1                               */
/* ------------------------------------------------------------------------- */

/**
  Program the PCIe Resizable BAR control register of BAR1 so that the firmware
  sizes BAR1 according to the selector.

  PciBusDxe calls this hook before it parses the BARs (PreprocessController(..
  EfiPciBeforeResourceCollection) runs in PciDetectDevice before PciSearchDevice
  parses the BARs), so the size programmed here is the size the firmware
  allocates.  This makes the BAR1 size independent of the ReBarState variable
  and of the generic ReBarUEFI resize pass.

  @retval TRUE   the control register now holds Selector + 6.
**/
STATIC
BOOLEAN
XveSetPciBar1Size (
  IN  UINTN   PciAddress,
  IN  UINT8   Selector,
  OUT UINT32  *CapMask,
  OUT UINT8   *OldSize,
  OUT UINT8   *NewSize,
  OUT UINT32  *CapPos
  )
{
  UINT16  Epos;
  INTN    Pos;
  UINT32  Cap;
  UINT32  Ctrl;
  UINT8   Want;

  *CapMask = 0;
  *OldSize = 0xFF;
  *NewSize = 0xFF;
  *CapPos  = 0;

  if (Selector == 0) {
    return FALSE;
  }

  Want = (UINT8)(Selector + 6U);   /* BAR1 size = 2^(selector+6) MiB */

  Epos = pciFindExtCapability (PciAddress, PCI_EXT_CAP_ID_REBAR);
  if (Epos == 0) {
    return FALSE;
  }

  /* pciRebarFindPos returns the position of the BAR's entry such that
          Pos + PCI_REBAR_CAP    = the "supported sizes" register of that BAR
          Pos + PCI_REBAR_CTRL   = the control register of that BAR
     (verified against lspci/pciutils output for this GPU: the supported sizes
     register of BAR1 is at 0xbbc, its control register at 0xbc0.) */
  Pos = pciRebarFindPos (PciAddress, (INTN)Epos, 1);
  if (Pos < 0) {
    return FALSE;
  }

  *CapPos = ((UINT32)Epos << 16) | ((UINT32)Pos & 0xFFFFU);

  if (EFI_ERROR (pciReadConfigDword (PciAddress, Pos + PCI_REBAR_CAP, &Cap))) {
    return FALSE;
  }

  *CapMask = (Cap & PCI_REBAR_CAP_SIZES) >> 4;

  if (EFI_ERROR (pciReadConfigDword (PciAddress, Pos + PCI_REBAR_CTRL, &Ctrl))) {
    return FALSE;
  }

  *OldSize = (UINT8)((Ctrl & PCI_REBAR_CTRL_BAR_SIZE) >> PCI_REBAR_CTRL_BAR_SHIFT);

  if ((*CapMask & (1U << Want)) == 0) {
    /* the device does not advertise the size (XVE write did not take) */
    return FALSE;
  }

  pciRebarSetSize (PciAddress, (UINTN)Epos, 1, Want);

  if (EFI_ERROR (pciReadConfigDword (PciAddress, Pos + PCI_REBAR_CTRL, &Ctrl))) {
    return FALSE;
  }

  *NewSize = (UINT8)((Ctrl & PCI_REBAR_CTRL_BAR_SIZE) >> PCI_REBAR_CTRL_BAR_SHIFT);

  return (BOOLEAN)(*NewSize == Want);
}

/* ------------------------------------------------------------------------- */
/* XVE register programming                                                  */
/* ------------------------------------------------------------------------- */

/**
  Signature check: the XVE block of a TU102 CMP 50HX answers with a ReBAR size
  bitmap (0x400 = 64 MiB only, before the unlock) at BAR0 + 0x88BBC.  Anything
  else means the address we are looking at is not the GPU, or nothing is decoded
  there (all-ones reads).

  @retval TRUE   the mapped window looks like the XVE block of the device.
**/
STATIC
BOOLEAN
XveProbe (
  IN  UINT64  Base,
  OUT XVE_RESULT  *Result
  )
{
  Result->CapOld = XveMmioRead32 (Base, XVE_CAP_OFFSET);
  Result->CfgOld = XveMmioRead32 (Base, XVE_CFG_OFFSET);
  Result->CyaOld = XveMmioRead32 (Base, XVE_CYA_OFFSET);

  if ((Result->CapOld == 0xFFFFFFFFU) || (Result->CapOld == 0)) {
    return FALSE;
  }

  if ((Result->CapOld & XVE_CAP_RESERVED_MASK) != 0) {
    return FALSE;
  }

  if ((Result->CapOld & XVE_CAP_MUST_BIT) == 0) {
    return FALSE;
  }

  return TRUE;
}

/**
  Perform the two XVE writes and verify them.

  @retval TRUE   writes were observed by the device (read back as programmed and
                 the requested BAR1 size is now advertised).
**/
STATIC
BOOLEAN
XveProgram (
  IN  UINT64      Base,
  IN  UINT8       Selector,
  OUT XVE_RESULT  *Result
  )
{
  UINT32  Expected;

  ZeroMem (Result, sizeof (*Result));

  if (!XveProbe (Base, Result)) {
    return FALSE;
  }

  Result->SignatureOk = TRUE;
  Expected            = (Result->CfgOld & ~(UINT32)XVE_CFG_SIZE_MASK) |
                        XVE_CFG_ENABLE | (UINT32)Selector;
  Result->CfgNew      = Expected;

  /* unlock the XVE password register, then configure the ReBAR selector */
  XveMmioWrite32 (Base, XVE_CYA_OFFSET, XVE_CYA_UNLOCK_VALUE);
  Result->CyaNew = XveMmioRead32 (Base, XVE_CYA_OFFSET);

  XveMmioWrite32 (Base, XVE_CFG_OFFSET, Expected);
  Result->CfgNew = XveMmioRead32 (Base, XVE_CFG_OFFSET);
  Result->CapNew = XveMmioRead32 (Base, XVE_CAP_OFFSET);

  if ((Result->CfgNew & (XVE_CFG_ENABLE | XVE_CFG_SIZE_MASK)) !=
      (Expected & (XVE_CFG_ENABLE | XVE_CFG_SIZE_MASK)))
  {
    /* device did not take the configuration, put the old values back */
    XveMmioWrite32 (Base, XVE_CFG_OFFSET, Result->CfgOld);
    XveMmioWrite32 (Base, XVE_CYA_OFFSET, Result->CyaOld);
    return FALSE;
  }

  if ((Result->CapNew == 0xFFFFFFFFU) ||
      ((Result->CapNew & (UINT32)(1U << (Selector + 10))) == 0))
  {
    /* the requested size is not advertised, roll back */
    XveMmioWrite32 (Base, XVE_CFG_OFFSET, Result->CfgOld);
    XveMmioWrite32 (Base, XVE_CYA_OFFSET, Result->CyaOld);
    return FALSE;
  }

  Result->Verified = TRUE;
  return TRUE;
}

/* ------------------------------------------------------------------------- */
/* temporary BAR0 remap                                                      */
/* ------------------------------------------------------------------------- */

/**
  Reserve a 16 MiB scratch window from the PCI memory-mapped I/O aperture.
  On this board the usable 32-bit PCI window ends at 0xFC00_0000, so the search
  starts there and walks down.
**/
STATIC
UINT64
XveAllocateScratch (
  VOID
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Address;

  Address = 0xFC000000ULL;
  Status  = gDS->AllocateMemorySpace (
                    EfiGcdAllocateMaxAddressSearchTopDown,
                    EfiGcdMemoryTypeMemoryMappedIo,
                    XVE_BAR0_ALIGN_SHIFT,
                    XVE_BAR0_SIZE,
                    &Address,
                    gImageHandle,
                    NULL
                    );
  if (!EFI_ERROR (Status) && (Address < 0x100000000ULL)) {
    gDS->SetMemorySpaceAttributes (Address, XVE_BAR0_SIZE, EFI_MEMORY_UC);
    return (UINT64)Address;
  }

  /* The PCI memory-mapped I/O aperture below 4 GiB is not described in the GCD
     on some firmware revisions.  Fall back to the top of the aperture observed
     on the BTC79X5 / X79ETH03 board (usable PCI window is 0xCC000000-0xFBFFFFFF);
     the XVE signature check decides whether anything is really decoded there. */
  DEBUG ((DEBUG_INFO, "ReBarDXE XVE: GCD scratch allocation failed, using %08x\n", (UINT32)XVE_SCRATCH_FALLBACK));
  return XVE_SCRATCH_FALLBACK;
}

/**
  Find the PCI-to-PCI bridge that is the parent of ChildBus.

  Bus numbers are handed out depth first, so the parent is normally at
  ChildBus - 1; the search walks down from there to remain correct when other
  bridges were enumerated in between.
**/
STATIC
BOOLEAN
XveFindParentBridge (
  IN  UINT8  ChildBus,
  OUT UINTN  *BridgeAddress,
  OUT UINT8  *BridgePrimaryBus
  )
{
  UINT8    Bus;
  UINT8    Device;
  UINT8    Function;
  UINT8    HeaderType;
  UINT8    PrimaryBus;
  UINT8    SecondaryBus;
  UINTN    Address;
  UINT32   Value;

  if (ChildBus == 0) {
    return FALSE;
  }

  for (Bus = (UINT8)(ChildBus - 1); Bus < ChildBus; Bus--) {
    for (Device = 0; Device <= 0x1F; Device++) {
      for (Function = 0; Function <= 0x07; Function++) {
        Address = EFI_PCI_ADDRESS (Bus, Device, Function, 0);

        if (EFI_ERROR (pciReadConfigDword (Address, PCI_VENDOR_ID, &Value))) {
          continue;
        }

        if ((Value & 0xFFFF) == 0xFFFF) {
          continue;
        }

        if (EFI_ERROR (pciReadConfigByte (Address, PCI_HEADER_TYPE, &HeaderType))) {
          continue;
        }

        if ((HeaderType & 0x7F) != 0x01) {
          continue;
        }

        if (EFI_ERROR (pciReadConfigByte (Address, PCI_SECONDARY_BUS, &SecondaryBus)) ||
            EFI_ERROR (pciReadConfigByte (Address, 0x18, &PrimaryBus)))
        {
          continue;
        }

        if (SecondaryBus == ChildBus) {
          *BridgeAddress    = Address;
          *BridgePrimaryBus = PrimaryBus;
          return TRUE;
        }
      }
    }

    if (Bus == 0) {
      break;
    }
  }

  return FALSE;
}

/**
  Widen the memory windows of every bridge between PciAddress and the root
  bridge so that [Start, Start + Size) is routed to the device.  The original
  registers are saved for XveCloseBridgeWindows().
**/
STATIC
UINTN
XveOpenBridgeWindows (
  IN  UINTN             PciAddress,
  IN  UINT64            Start,
  IN  UINT64            Size,
  OUT XVE_BRIDGE_STATE  *Bridges,
  IN  UINTN             MaxBridges
  )
{
  UINTN    Count;
  UINT8    ChildBus;
  UINT8    PrimaryBus;
  UINTN    Address;
  UINT32   Value;
  UINT32   Window;

  if (Start + Size > 0x100000000ULL) {
    return 0; /* only the 32-bit bridge window encoding is supported here */
  }

  Window = (UINT32)((Start & 0xFFF0) | (((Start + Size - 1) & 0xFFF0) << 16));

  ChildBus = (UINT8)((PciAddress & 0xFF000000) >> 24);
  Count    = 0;

  while (Count < MaxBridges) {
    if (!XveFindParentBridge (ChildBus, &Address, &PrimaryBus)) {
      break;
    }

    Bridges[Count].Address = Address;

    if (EFI_ERROR (pciReadConfigDword (Address, PCI_COMMAND, &Value))) {
      break;
    }

    Bridges[Count].Command = Value;

    if (EFI_ERROR (pciReadConfigDword (Address, 0x20, &Value))) {
      break;
    }

    Bridges[Count].MemBaseLimit = Value;

    if (EFI_ERROR (pciReadConfigDword (Address, 0x24, &Value))) {
      break;
    }

    Bridges[Count].PrefBaseLimit = Value;

    if (EFI_ERROR (pciReadConfigDword (Address, 0x28, &Value))) {
      break;
    }

    Bridges[Count].PrefBaseUpper = Value;

    if (EFI_ERROR (pciReadConfigDword (Address, 0x2C, &Value))) {
      break;
    }

    Bridges[Count].PrefLimitUpper = Value;

    /* non-prefetchable and prefetchable window: cover the scratch range */
    Value = Window;
    pciWriteConfigDword (Address, 0x20, &Value);
    pciWriteConfigDword (Address, 0x24, &Value);

    Value = 0;
    pciWriteConfigDword (Address, 0x28, &Value);
    pciWriteConfigDword (Address, 0x2C, &Value);

    pciReadConfigDword (Address, PCI_COMMAND, &Value);
    Value |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    pciWriteConfigDword (Address, PCI_COMMAND, &Value);

    DEBUG ((
      DEBUG_INFO,
      "ReBarDXE XVE: bridge %02x:%02x.%x window opened to %08x\n",
      (UINTN)((Address >> 24) & 0xFF),
      (UINTN)((Address >> 16) & 0x1F),
      (UINTN)((Address >> 8) & 0x07),
      (UINT32)Start
      ));

    Count++;

    if (PrimaryBus == 0 || PrimaryBus >= ChildBus) {
      break;
    }

    ChildBus = PrimaryBus;
  }

  return Count;
}

STATIC
VOID
XveCloseBridgeWindows (
  IN XVE_BRIDGE_STATE  *Bridges,
  IN UINTN             Count
  )
{
  UINTN   Index;
  UINT32  Value;

  for (Index = 0; Index < Count; Index++) {
    Value = Bridges[Index].MemBaseLimit;
    pciWriteConfigDword (Bridges[Index].Address, 0x20, &Value);
    Value = Bridges[Index].PrefBaseLimit;
    pciWriteConfigDword (Bridges[Index].Address, 0x24, &Value);
    Value = Bridges[Index].PrefBaseUpper;
    pciWriteConfigDword (Bridges[Index].Address, 0x28, &Value);
    Value = Bridges[Index].PrefLimitUpper;
    pciWriteConfigDword (Bridges[Index].Address, 0x2C, &Value);
    Value = Bridges[Index].Command;
    pciWriteConfigDword (Bridges[Index].Address, PCI_COMMAND, &Value);
  }
}

/**
  Temporarily point BAR0 of the device at a scratch window, run the XVE
  programming, then restore BAR0, the command register and the bridge windows.
**/
STATIC
BOOLEAN
XveRemapAndProgram (
  IN  UINTN       PciAddress,
  IN  UINT8       Selector,
  IN  BOOLEAN     Bar0Is64Bit,
  OUT UINT64      *UsedBase,
  OUT XVE_RESULT  *Result
  )
{
  UINT64            Scratch;
  UINT32            Bar0Save;
  UINT32            Bar0HiSave;
  UINT32            CommandSave;
  UINT32            Value;
  UINTN             BridgeCount;
  BOOLEAN           Ok;
  XVE_BRIDGE_STATE  Bridges[XVE_MAX_BRIDGES];

  Scratch = XveAllocateScratch ();
  if (Scratch == 0) {
    return FALSE;
  }

  *UsedBase = Scratch;

  BridgeCount = XveOpenBridgeWindows (PciAddress, Scratch, XVE_BAR0_SIZE, Bridges, XVE_MAX_BRIDGES);

  if (EFI_ERROR (pciReadConfigDword (PciAddress, PCI_COMMAND, &CommandSave)) ||
      EFI_ERROR (pciReadConfigDword (PciAddress, PCI_BASE_ADDRESS_0, &Bar0Save)) ||
      EFI_ERROR (pciReadConfigDword (PciAddress, 0x14, &Bar0HiSave)))
  {
    XveCloseBridgeWindows (Bridges, BridgeCount);
    return FALSE;
  }

  /* stop decoding before touching BAR0 */
  Value = CommandSave & ~(UINT32)PCI_COMMAND_MEMORY;
  pciWriteConfigDword (PciAddress, PCI_COMMAND, &Value);

  Value = (UINT32)Scratch & 0xFFFFFFF0;
  pciWriteConfigDword (PciAddress, PCI_BASE_ADDRESS_0, &Value);

  if (Bar0Is64Bit) {
    Value = (UINT32)(Scratch >> 32);
    pciWriteConfigDword (PciAddress, 0x14, &Value);
  }

  Value = CommandSave | PCI_COMMAND_MEMORY;
  pciWriteConfigDword (PciAddress, PCI_COMMAND, &Value);

  Ok = XveProgram (Scratch, Selector, Result);

  /* restore config space; the XVE state itself lives in the device and stays */
  Value = CommandSave & ~(UINT32)PCI_COMMAND_MEMORY;
  pciWriteConfigDword (PciAddress, PCI_COMMAND, &Value);

  Value = Bar0Save;
  pciWriteConfigDword (PciAddress, PCI_BASE_ADDRESS_0, &Value);

  if (Bar0Is64Bit) {
    Value = Bar0HiSave;
    pciWriteConfigDword (PciAddress, 0x14, &Value);
  }

  Value = CommandSave;
  pciWriteConfigDword (PciAddress, PCI_COMMAND, &Value);

  XveCloseBridgeWindows (Bridges, BridgeCount);

  gDS->FreeMemorySpace (Scratch, XVE_BAR0_SIZE);

  return Ok;
}

/* ------------------------------------------------------------------------- */
/* entry point of the pre-pass                                               */
/* ------------------------------------------------------------------------- */

VOID
XveTu102Prepass (
  IN UINTN   PciAddress,
  IN UINT16  VendorId,
  IN UINT16  DeviceId,
  IN UINT8   ReBarState
  )
{
  XVE_RESULT  Result;
  UINT32      Bar0;
  UINT32      Bar0Hi;
  UINT32      Command;
  UINT8       Selector;
  UINT64      Base;
  UINTN       Index;
  BOOLEAN     Bar0Is64Bit;
  BOOLEAN     Ok;
  BOOLEAN     RemapUsed;
  BOOLEAN     FromVariable;
  BOOLEAN     PcieOk;
  UINT32      CapMask;
  UINT32      CapPos;
  UINT8       OldSize;
  UINT8       NewSize;

  if ((VendorId != XVE_VENDOR_ID) || (DeviceId != XVE_DEVICE_ID)) {
    return;
  }

  for (Index = 0; Index < mXveHandledCount; Index++) {
    if (mXveHandled[Index] == PciAddress) {
      return;
    }
  }

  if (mXveHandledCount < ARRAY_SIZE (mXveHandled)) {
    mXveHandled[mXveHandledCount++] = PciAddress;
  }

  /* v2: the pre-pass is *not* gated on the ReBarState variable any more.  The
     generic ReBarUEFI resize pass still is, but a freshly flashed image has no
     variables at all (flashing rewrites the variable store), and that is exactly
     the situation the first boot after a flash is in. */
  Selector   = XveGetSelector (&FromVariable);
  RemapUsed  = FALSE;
  Ok         = FALSE;

  if (Selector == XVE_SELECTOR_OFF) {
    ZeroMem (&Result, sizeof (Result));
    XveLogDevice (L"off", PciAddress, 0, FromVariable, 0, &Result);
    return;
  }

  ZeroMem (&Result, sizeof (Result));

  if (EFI_ERROR (pciReadConfigDword (PciAddress, PCI_BASE_ADDRESS_0, &Bar0))) {
    return;
  }

  if (EFI_ERROR (pciReadConfigDword (PciAddress, 0x14, &Bar0Hi))) {
    Bar0Hi = 0;
  }

  if (EFI_ERROR (pciReadConfigDword (PciAddress, PCI_COMMAND, &Command))) {
    Command = 0;
  }

  Bar0Is64Bit = (BOOLEAN)((Bar0 & 0x06) == 0x04);
  Base        = (UINT64)(Bar0 & 0xFFFFFFF0);
  if (Bar0Is64Bit) {
    Base |= ((UINT64)Bar0Hi << 32);
  }

  /* step 1: use BAR0 as the firmware has it */
  if (Base != 0) {
    Ok = XveProgram (Base, Selector, &Result);
    if (!Ok && (Command & PCI_COMMAND_MEMORY) == 0) {
      UINT32  Value = Command | PCI_COMMAND_MEMORY;

      pciWriteConfigDword (PciAddress, PCI_COMMAND, &Value);
      Ok = XveProgram (Base, Selector, &Result);
      Value = Command;
      pciWriteConfigDword (PciAddress, PCI_COMMAND, &Value);
    }
  }

  /* step 2: BAR0 is not decoded yet, remap it temporarily */
  if (!Ok) {
    UINT64  Scratch = 0;

    ZeroMem (&Result, sizeof (Result));
    RemapUsed = TRUE;
    Ok        = XveRemapAndProgram (PciAddress, Selector, Bar0Is64Bit, &Scratch, &Result);
    Base      = Scratch;
  }

  XveLogDevice (
    Ok ? (RemapUsed ? L"remap-ok" : L"bar0-ok") : (RemapUsed ? L"remap-fail" : L"bar0-fail"),
    PciAddress,
    Selector,
    FromVariable,
    Base,
    &Result
    );

  /* Hand the advertised size to the firmware.  PciBusDxe calls this hook from
     PciDetectDevice() *before* PciSearchDevice() parses the BARs, therefore the
     size programmed here is the size the firmware allocates for BAR1. */
  if (Ok) {
    CapMask = 0;
    OldSize = 0xFF;
    NewSize = 0xFF;
    CapPos  = 0;
    PcieOk  = XveSetPciBar1Size (PciAddress, Selector, &CapMask, &OldSize, &NewSize, &CapPos);
    XveLogPcie (
      PcieOk ? L"ok" : L"fail",
      PciAddress,
      Selector,
      CapMask,
      OldSize,
      NewSize,
      CapPos,
      PcieOk
      );
  }
}
