/*
Copyright (c) 2022-2023 xCuri0 <zkqri0@gmail.com>
SPDX-License-Identifier: MIT
*/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/PciRootBridgeIo.h>
#include <IndustryStandard/Pci22.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include "include/pciRegs.h"
#include "include/PciHostBridgeResourceAllocation.h"
#include "include/XveTu102.h"

#ifdef _MSC_VER
#pragma warning(disable:28251)
#include <intrin.h>
#pragma warning(default:28251)
#endif

#define PCI_POSSIBLE_ERROR(val) ((val) == 0xffffffff)

// for quirk
#define PCI_VENDOR_ID_ATI 0x1002

// if system time is before this year then CMOS reset will be detected and rebar will be disabled.
#define BUILD_YEAR 2023

// a3c5b77a-c88f-4a93-bf1c-4a92a32c65ce
static GUID reBarStateGuid = { 0xa3c5b77a, 0xc88f, 0x4a93, {0xbf, 0x1c, 0x4a, 0x92, 0xa3, 0x2c, 0x65, 0xce}};

// 0: disabled
// >0: maximum BAR size (2^x) set to value. UINT8_MAX for unlimited
static UINT8 reBarState = 0;

static EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL *pciResAlloc;
static EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *pciRootBridgeIo;

static EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL_PREPROCESS_CONTROLLER o_PreprocessController;

// find last set bit and return the index of it
INTN fls(UINT32 x)
{
    UINT32 r;

    #ifdef _MSC_VER
    _BitScanReverse64(&r, x);
    #else
    // taken from linux x86 bitops.h
    asm("bsrl %1,%0"
	    : "=r" (r)
	    : "rm" (x), "0" (-1));
    #endif

    return r;
}

UINT64 pciAddrOffset(UINTN pciAddress, INTN offset)
{
    UINTN reg = (pciAddress & 0xffffffff00000000) >> 32;
    UINTN bus = (pciAddress & 0xff000000) >> 24;
    UINTN dev = (pciAddress & 0xff0000) >> 16;
    UINTN func = (pciAddress & 0xff00) >> 8;

    return EFI_PCI_ADDRESS(bus, dev, func, ((INT64)reg + offset));
}

// created these functions to make it easy to read as we are adapting alot of code from Linux
EFI_STATUS pciReadConfigDword(UINTN pciAddress, INTN pos, UINT32 *buf)
{
    return pciRootBridgeIo->Pci.Read(pciRootBridgeIo, EfiPciWidthUint32, pciAddrOffset(pciAddress, pos), 1, buf);
}

EFI_STATUS pciWriteConfigDword(UINTN pciAddress, INTN pos, UINT32 *buf)
{
    return pciRootBridgeIo->Pci.Write(pciRootBridgeIo, EfiPciWidthUint32, pciAddrOffset(pciAddress, pos), 1, buf);
}
EFI_STATUS pciReadConfigWord(UINTN pciAddress, INTN pos, UINT16 *buf)
{
    return pciRootBridgeIo->Pci.Read(pciRootBridgeIo, EfiPciWidthUint16, pciAddrOffset(pciAddress, pos), 1, buf);
}

EFI_STATUS pciWriteConfigWord(UINTN pciAddress, INTN pos, UINT16 *buf)
{
    return pciRootBridgeIo->Pci.Write(pciRootBridgeIo, EfiPciWidthUint16, pciAddrOffset(pciAddress, pos), 1, buf);
}

EFI_STATUS pciReadConfigByte(UINTN pciAddress, INTN pos, UINT8 *buf)
{
    return pciRootBridgeIo->Pci.Read(pciRootBridgeIo, EfiPciWidthUint8, pciAddrOffset(pciAddress, pos), 1, buf);
}

EFI_STATUS pciWriteConfigByte(UINTN pciAddress, INTN pos, UINT8 *buf)
{
    return pciRootBridgeIo->Pci.Write(pciRootBridgeIo, EfiPciWidthUint8, pciAddrOffset(pciAddress, pos), 1, buf);
}

// adapted from linux pci_find_ext_capability
UINT16 pciFindExtCapability(UINTN pciAddress, INTN cap)
{
    INTN ttl;
    UINT32 header;
    UINT16 pos = PCI_CFG_SPACE_SIZE;

    /* minimum 8 bytes per capability */
    ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;

    if (EFI_ERROR(pciReadConfigDword(pciAddress, pos, &header)))
        return 0;
    /*
     * If we have no capabilities, this is indicated by cap ID,
     * cap version and next pointer all being 0. Or it could also be all FF
     */
    if (header == 0 || PCI_POSSIBLE_ERROR(header))
        return 0;

    while (ttl-- > 0)
    {
        if (PCI_EXT_CAP_ID(header) == cap && pos != 0)
            return pos;

        pos = PCI_EXT_CAP_NEXT(header);
        if (pos < PCI_CFG_SPACE_SIZE)
            break;

        if (EFI_ERROR(pciReadConfigDword(pciAddress, pos, &header)))
            break;
    }
    return 0;
}

INTN pciRebarFindPos(UINTN pciAddress, INTN pos, UINT8 bar)
{
    UINTN nbars, i;
    UINT32 ctrl;

    pciReadConfigDword(pciAddress, pos + PCI_REBAR_CTRL, &ctrl);
    nbars = (ctrl & PCI_REBAR_CTRL_NBAR_MASK) >>
            PCI_REBAR_CTRL_NBAR_SHIFT;

    for (i = 0; i < nbars; i++, pos += 8)
    {
        UINTN bar_idx;

        pciReadConfigDword(pciAddress, pos + PCI_REBAR_CTRL, &ctrl);
        bar_idx = ctrl & PCI_REBAR_CTRL_BAR_IDX;
        if (bar_idx == bar)
            return pos;
    }
    return -1;
}

UINT32 pciRebarGetPossibleSizes(UINTN pciAddress, UINTN epos, UINT16 vid, UINT16 did, UINT8 bar)
{
    INTN pos;
    UINT32 cap;

    pos = pciRebarFindPos(pciAddress, (INTN)epos, bar);
    if (pos < 0)
        return 0;

    pciReadConfigDword(pciAddress, pos + PCI_REBAR_CAP, &cap);
    cap &= PCI_REBAR_CAP_SIZES;

    /* Sapphire RX 5600 XT Pulse has an invalid cap dword for BAR 0 */
    if (vid == PCI_VENDOR_ID_ATI && did == 0x731f &&
        bar == 0 && cap == 0x7000)
        cap = 0x3f000;

    return cap >> 4;
}

INTN pciRebarSetSize(UINTN pciAddress, UINTN epos, UINT8 bar, UINT8 size)
{
    INTN pos;
    UINT32 ctrl;

    pos = pciRebarFindPos(pciAddress, (INTN)epos, bar);
    if (pos < 0)
        return pos;

    pciReadConfigDword(pciAddress, pos + PCI_REBAR_CTRL, &ctrl);
    ctrl &= (UINT32)~PCI_REBAR_CTRL_BAR_SIZE;
    ctrl |= (UINT32)size << PCI_REBAR_CTRL_BAR_SHIFT;

    pciWriteConfigDword(pciAddress, pos + PCI_REBAR_CTRL, &ctrl);
    return 0;
}

VOID reBarSetupDevice(EFI_HANDLE handle, EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS addrInfo)
{
    UINTN epos;
    UINT16 vid, did;
    UINTN pciAddress;

    gBS->HandleProtocol(handle, &gEfiPciRootBridgeIoProtocolGuid, (void **)&pciRootBridgeIo);

    pciAddress = EFI_PCI_ADDRESS(addrInfo.Bus, addrInfo.Device, addrInfo.Function, 0);
    pciReadConfigWord(pciAddress, 0, &vid);
    pciReadConfigWord(pciAddress, 2, &did);

    if (vid == 0xFFFF)
        return;

    DEBUG((DEBUG_INFO, "ReBarDXE: Device vid:%x did:%x\n", vid, did));

    // XVE pre-pass for TU102 CMP 50HX (10de:1e09): unlock the large BAR1 sizes
    // inside the card BEFORE the resize pass below hands them to the firmware.
    XveTu102Prepass(pciAddress, vid, did, reBarState);

    epos = pciFindExtCapability(pciAddress, PCI_EXT_CAP_ID_REBAR);
    if (epos)
    {
        for (UINT8 bar = 0; bar < 6; bar++)
        {
            UINT32 rBarS = pciRebarGetPossibleSizes(pciAddress, epos, vid, did, bar);
            if (!rBarS)
                continue;
            // start with size from fls
            for (UINT8 n = MIN((UINT8)fls(rBarS), reBarState); n > 0; n--) {
                // check if size is supported
                if (rBarS & (1 << n)) {
                    pciRebarSetSize(pciAddress, epos, bar, n);
                    break;
                }
            }
        }
    }
}

static BOOLEAN reBarHookCalled = FALSE;

EFI_STATUS EFIAPI PreprocessControllerOverride (
  IN  EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PROTOCOL  *This,
  IN  EFI_HANDLE                                        RootBridgeHandle,
  IN  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS       PciAddress,
  IN  EFI_PCI_CONTROLLER_RESOURCE_ALLOCATION_PHASE      Phase
  )
{
    // call the original method
    EFI_STATUS status = o_PreprocessController(This, RootBridgeHandle, PciAddress, Phase);

    DEBUG((DEBUG_INFO, "ReBarDXE: Hooked PreprocessController called %d\n", Phase));

    // one-time marker: proves that the hook is really in the call path, written
    // from a context in which the UEFI variable services are up for sure
    if (!reBarHookCalled) {
        CHAR16 line[96];

        reBarHookCalled = TRUE;
        UnicodeSPrint(line, sizeof(line), L"[hook] PreprocessController called, phase=%d", (UINTN)Phase);
        XveLogLine(line);
    }

    // EDK2 PciBusDxe setups Resizable BAR twice so we will do same
    if (Phase <= EfiPciBeforeResourceCollection) {
        reBarSetupDevice(RootBridgeHandle, PciAddress);
    }

    return status;
}

static BOOLEAN reBarHookInstalled = FALSE;
static EFI_EVENT reBarNotifyEvent = NULL;
static VOID *reBarNotifyRegistration = NULL;

VOID EFIAPI pciHostBridgeResourceAllocationNotify(EFI_EVENT event, VOID *context);

VOID pciHostBridgeResourceAllocationProtocolHook()
{
    EFI_STATUS status;
    UINTN handleCount;
    EFI_HANDLE *handleBuffer = NULL;

    if (reBarHookInstalled)
        return;

    status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiPciHostBridgeResourceAllocationProtocolGuid,
        NULL,
        &handleCount,
        &handleBuffer);

    if (EFI_ERROR(status) || handleCount == 0) {
        // The PCI host bridge has not published its resource allocation protocol
        // yet. It is installed when the host bridge driver is started, which can
        // happen after this driver has been dispatched (the Boot Device Selection
        // phase connects the host bridge). Register a protocol notify so that the
        // hook is still installed before the PCI bus driver enumerates and hands
        // out the BAR sizes.
        if (reBarNotifyEvent == NULL) {
            status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_NOTIFY,
                pciHostBridgeResourceAllocationNotify, NULL, &reBarNotifyEvent);

            if (!EFI_ERROR(status)) {
                status = gBS->RegisterProtocolNotify(
                    &gEfiPciHostBridgeResourceAllocationProtocolGuid,
                    reBarNotifyEvent,
                    &reBarNotifyRegistration);

                DEBUG((DEBUG_INFO, "ReBarDXE: Host bridge protocol not up yet, notify registered %r\n", status));
                XveLogLine(L"[hook] host bridge protocol absent, protocol notify registered");
            }
        }
        goto free;
    }

    status = gBS->OpenProtocol(
        handleBuffer[0],
        &gEfiPciHostBridgeResourceAllocationProtocolGuid,
        (VOID **)&pciResAlloc,
        gImageHandle,
        NULL,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL);

    if (EFI_ERROR(status))
        goto free;

    DEBUG((DEBUG_INFO, "ReBarDXE: Hooking EfiPciHostBridgeResourceAllocationProtocol->PreprocessController\n"));

    // Hook PreprocessController
    o_PreprocessController = pciResAlloc->PreprocessController;
    pciResAlloc->PreprocessController = &PreprocessControllerOverride;
    reBarHookInstalled = TRUE;
    XveLogLine(L"[hook] EfiPciHostBridgeResourceAllocationProtocol->PreprocessController hooked");

free:
    if (handleBuffer != NULL) {
        FreePool(handleBuffer);
    }
}

VOID EFIAPI pciHostBridgeResourceAllocationNotify(EFI_EVENT event, VOID *context)
{
    pciHostBridgeResourceAllocationProtocolHook();
}

EFI_STATUS EFIAPI rebarInit(
    IN EFI_HANDLE imageHandle,
    IN EFI_SYSTEM_TABLE *systemTable)
{
    UINTN bufferSize = 1;
    EFI_STATUS status;
    UINT32 attributes;
    EFI_TIME time;
    CHAR16 line[96];
    UINT8 xveSelector;
    UINTN xveSize = 1;

    DEBUG((DEBUG_INFO, "ReBarDXE: Loaded\n"));

    // Read ReBarState variable
    status = gRT->GetVariable(L"ReBarState", &reBarStateGuid,
        &attributes,
        &bufferSize, &reBarState);

    // any attempts to overflow reBarState should result in EFI_BUFFER_TOO_SMALL
    if (status != EFI_SUCCESS)
        reBarState = 0;

    // v2 entry marker.  This line is written before anything else and without any
    // condition: if the XvePrepassLog variable is missing after a boot then this
    // module was not dispatched at all, which is a different failure from the
    // pre-pass itself not being called.
    UnicodeSPrint(line, sizeof(line),
        L"[entry] ReBarDxe-XVE v2 (nobar1=1 default, 16GiB via XveBar1Selector=8) state=%d",
        (UINTN)reBarState);
    XveLogLine(line);

    xveSelector = 0;
    if (!EFI_ERROR(gRT->GetVariable(L"XveBar1Selector", &reBarStateGuid, &attributes, &xveSize, &xveSelector)))
    {
        UnicodeSPrint(line, sizeof(line), L"[entry] XveBar1Selector variable = %d", (UINTN)xveSelector);
    }
    else
    {
        UnicodeSPrint(line, sizeof(line), L"[entry] XveBar1Selector variable absent, using default");
    }

    XveLogLine(line);

    if (reBarState)
    {
        DEBUG((DEBUG_INFO, "ReBarDXE: Enabled, maximum BAR size 2^%u MB\n", reBarState));

        // Detect CMOS reset by checking if year before BUILD_YEAR
        status = gRT->GetTime (&time, NULL);
        if (time.Year < BUILD_YEAR) {
            reBarState = 0;
            bufferSize = 1;
            attributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;

            status = gRT->SetVariable(L"ReBarState", &reBarStateGuid,
                attributes,
                bufferSize, &reBarState);

            XveLogLine(L"[entry] year < BUILD_YEAR, ReBarState reset to 0 (CMOS reset detected)");
        }
    }

    // For overriding PciHostBridgeResourceAllocationProtocol.
    //
    // v2: the hook is installed unconditionally, independently of ReBarState.
    // The XVE pre-pass must also run when the ReBarState variable is missing,
    // which is what a freshly flashed image looks like: flashing rewrites the
    // variable store, so on the first boot after a flash there are no variables
    // at all.  With reBarState == 0 the generic resize pass below is a no-op.
    pciHostBridgeResourceAllocationProtocolHook();

    return EFI_SUCCESS;
}
