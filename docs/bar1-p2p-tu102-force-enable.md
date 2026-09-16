# BAR1 P2P force-enable on pre-Hopper CMP cards (610.43.03)

Mailbox P2P is a dead end on pre-Hopper CMP cards: an unallocated mailbox area turns peer DMA into host-RAM corruption. The correct path is BAR1 P2P, which the 610 driver implements natively but never selects for GeForce/Turing. Enabling it is a small set of force-enables, not a new data path.

## The four required pieces

1. Route the default (pre-Hopper) HAL to the GH100 BAR1-P2P implementations in the generated HAL table (`src/nvidia/generated/g_kern_bus_nvoc.c`): `kbusCreateP2PMapping`, `kbusIsPcieBar1P2PMappingSupported`, `kbusCreateP2PMappingForBar1P2P`, etc. The GH100 implementations are chip-independent (refcounting + IOMMU mappings, no Hopper registers).
2. Skip mailbox peer pre-registration. `_kbusInitP2P_GM107` (kern_bus_gm107.c) assigns a mailbox peer id for every GPU pair at init, leaving `p2pPcie.peerNumberMask` non-zero; `kbusIsPcieBar1P2PMappingSupported_GH100` refuses BAR1 P2P when a mailbox peer already exists. Guard the `_kbusInitP2P_GM107` call so it does not run when `pcieP2PType == BAR1` (bayley patch 0013).
3. Force the BAR1-P2P read cap. `_p2pCapsGetHostSystemStatusOverPcieBar1` grants the read cap only for a common PCIe switch / Ryzen / Xeon-SPR; anything else gets CHIPSET_NOT_SUPPORTED and the driver falls back to mailbox. Override it for CMP device IDs only (`pGpu->idInfo.PCIDeviceID >> 16` == 0x1E09 = CMP 50HX, 0x20C2 = 170HX, 0x2082 = 90HX) (bayley patch 0015).
4. Static BAR1 covering the client-visible framebuffer (32 GiB for a 20 GiB CMP), gated by `RMPcieP2PType=1` + `RMForceStaticBar1=1`, with IOMMU passthrough (`iommu=pt`) so the peer DMA is not translated.

The aikitoria 610.43.03-p2p fork already carries pieces 1 plus `pcieP2PType=BAR1` default and `p2pOverride=0x11` in `kernel_bif.c`; it is missing 2 and 3. Reference patches for 610.43.03: bayley/cmpunlocker `driver/patches/0013-skip-mailbox-peer-preinit.patch` and `0015-bar1p2p-readcap-override.patch` (add 0x1E09 to the device-id list — bayley's list covers 0x20C2/0x2082 only).

## The firmware gate

Full-FB BAR1 P2P is gated by static BAR1, which needs a 32 GiB BAR1 aperture. On BTC79X5 the XVE pre-pass delivers 16 GiB (selector 8) reliably but selector 9 (32 GiB) wedges POST — the host never returns ("No route to host" while the NIC stays down). So the driver recipe alone is not sufficient on that board: the 32 GiB firmware aperture is the actual blocker, and it is a POST/firmware problem, not a driver problem. Do not chase driver fixes when the aperture cannot be brought up.

## Verify the patch landed in the binary

A source edit that applies and compiles cleanly (BUILD_EXIT=0) can still be inert. Check the artifact, not the source:

```bash
# the added debug string must be in the final module
strings kernel-open/nvidia.ko | grep -i 'CMPUNLOCK_BAR1P2P'
# the patched function's symbol must exist in its object file
nm -a src/nvidia/_out/Linux_x86_64/kern_bus_gm107.o | grep -i '_kbusInitP2P_GM107'
```

If the symbol and string are absent despite a clean build, the patched call site sits in a code path `#if`'d out of the target chip's build (the function was never compiled for that die). TU102's mailbox init may not run through `_kbusInitP2P_GM107` at all — its mailbox write path is gm200 (`kern_bus_gm200.c`), so confirm which arch file the target die actually exercises before trusting a skip-mailbox patch aimed at the wrong generation.
