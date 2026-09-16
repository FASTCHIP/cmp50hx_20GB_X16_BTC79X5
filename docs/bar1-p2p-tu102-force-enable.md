# BAR1 P2P force-enable on pre-Hopper CMP cards (610.43.03)

Mailbox P2P is a dead end on pre-Hopper CMP cards when the driver is forced to BAR1
mode: an unallocated mailbox area turns peer DMA into host-RAM corruption. The correct
path is BAR1 P2P, which the 610 driver implements natively but never selects for
GeForce/Turing. Enabling it is a small set of force-enables, not a new data path.

## What the aikitoria fork already does (no source patches needed)

The `aikitoria/610.43.03-p2p` fork ships everything driver-side:

1. Routes the default (pre-Hopper) HAL to the GH100 BAR1-P2P implementations in the
   generated HAL table (`src/nvidia/generated/g_kern_bus_nvoc.c`):
   `kbusCreateP2PMappingForBar1P2P`, `kbusIsPcieBar1P2PMappingSupported`, etc. The GH100
   implementations are chip-independent (refcounting + IOMMU mappings, no Hopper
   registers — verified by reading `kern_bus_gh100.c`).
2. Defaults `pcieP2PType=BAR1`.
3. Sets `p2pOverride=0x11` (READ_ENABLE + WRITE_ENABLE) in `kernel_bif.c`, which is the
   key: the read cap is forced by the override, so the read-cap patch below is redundant.

The two extra patches from the bayley/cmpunlocker set are NOT needed on this build:

- **0013 (skip mailbox peer pre-registration) is a compile-time no-op.** The guard
  `gpumgrGetGpuLinkCount()` is `#define ... ((NvU32) 0)` in this build, so
  `_kbusInitP2P_GM107` (which assigns a mailbox peer id and leaves
  `p2pPcie.peerNumberMask` non-zero) is constant-folded dead. `peerNumberMask` is
  already clear, so `kbusIsPcieBar1P2PMappingSupported_GH100`'s "no mailbox peer" check
  passes without any patch.
- **0015 (force the read cap) is redundant.** `p2pOverride=0x11` decodes to
  READ_ENABLE (bits 1:0 = 01) + WRITE_ENABLE (bits 5:4 = 01). Inside
  `_kp2pCapsGetStatusOverPcieBar1` (p2p_caps.c), `_kp2pCapsCheckStatusOverridesForPcie`
  runs BEFORE `_p2pCapsGetHostSystemStatusOverPcieBar1` (the 0015 target), so the
  override short-circuits and the 0015-patched function is never reached — in every
  configuration (static BAR1 enabled: override wins; static BAR1 absent: the
  `kbusIsPcieBar1P2PMappingSupported_HAL` check fails first).

## The only remaining gate: static BAR1 (RESOLVED 2026-09-16)

Full-FB BAR1 P2P is gated by static BAR1 covering the client-visible framebuffer
(32 GiB for a 20 GiB CMP), gated by `RMForceStaticBar1=1` (NOT `RMPcieP2PType=1` — the
fork already defaults `pcieP2PType=BAR1`), with IOMMU passthrough (`iommu=pt`) so the
peer DMA is not translated.

On BTC79X5 the XVE pre-pass delivers 16 GiB (selector 8) reliably. Selector 9 (32 GiB)
originally wedged POST — but the root cause was NOT the selector code, it was the default
64 GiB MMIOH pool (2×32 GiB cards need ~96 GiB of contiguous prefetchable window, and the
second card landed outside the 64 GiB pool). Enlarging MMIOH 64→256 GiB (1-byte code
patch, `firmware/PATCH_MMIOH_256G.md`) resolves it: selector 9 boots with
`Region 1 [size=32G]` on both cards and full 20480 MiB VRAM. Verified 2026-09-16: direct
P2P `cudaMemcpyPeerAsync` 2.47 GB/s bidirectional, 0 integrity mismatches, host RAM clean.


## Why the mailbox fallback corrupts RAM (and how to avoid it)

With `pcieP2PType=BAR1` but static BAR1 NOT enabled, `kbusIsPcieBar1P2PMappingSupported_HAL`
returns NV_FALSE, so `_kp2pCapsGetStatusOverPcieBar1` returns NOT_SUPPORTED and
`p2pGetCapsStatus` degrades connectivity to PCIE_PROPRIETARY (mailbox). The mailbox path
then runs with a peer state that was never pre-registered (dead-coded `_kbusInitP2P_GM107`),
and under `iommu=pt` the bogus DMA corrupts host RAM silently instead of faulting.

Stock Turing mailbox P2P is fine; the fork's broken fallback is not. The corruption is
therefore AVOIDABLE: enable static BAR1 BEFORE loading the fork, so the driver selects
PCIE_BAR1 and never touches the mailbox fallback. `iommu=pt` is required for BAR1 P2P to
work (translated mode fails the transfer) but is also what makes any mapping bug silent
corruption — test with a content-verifying integrity check, trusted software only, and
physical power-cycle access. Keep the IOMMU translated while testing any patched peer path.

## Topology: separate root ports are fine on this board

Root ports measured on ai100gb: 8086:3c02 (Root Port 1a) and 8086:3c08 (Root Port 3a),
both inside the driver's own recognized IOH range `DEVICE_ID_INTEL_3C02..3C0B` in
`_kp2pCapsGetStatusOverPcie` (p2p_caps.c). That function's comment states verbatim
"PCI-E P2P transactions ARE forwarded between different root ports implemented within a
given Intel I/O hub". Same IIO (domain 0, bus 00) ⇒ peer MMIO is forwarded across the
root ports. P2P fails only between different IOH/sockets (QPI).

## Verify the patch landed in the binary

A source edit that applies and compiles cleanly (BUILD_EXIT=0) can still be inert. Check
the artifact, not the source:

```bash
# the added debug string must be in the final module
strings kernel-open/nvidia.ko | grep -i 'CMPUNLOCK_BAR1P2P'
# the patched function's symbol must exist in its object file
nm -a src/nvidia/_out/Linux_x86_64/kern_bus_gm107.o | grep -i '_kbusInitP2P_GM107'
```

If the symbol and string are absent despite a clean build, the patched call site sits in
a code path `#if`'d out of the target chip's build (the function was never compiled for
that die). TU102's mailbox init does not run through `_kbusInitP2P_GM107` at all
(confirmed: `gpumgrGetGpuLinkCount` ≡ 0 folds it dead) — its mailbox write path is gm200
(`kern_bus_gm200.c`), so a skip-mailbox patch aimed at gm107 was never doing anything on
this die. This is exactly why 0013 is a no-op.
