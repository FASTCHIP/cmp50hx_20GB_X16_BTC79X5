# CMP 50HX 20GB × N on BTC79X5 (X79) — ReBAR / MMIOH / P2P

Verified, working firmware mods and scripts for running multiple NVIDIA CMP 50HX
(20 GiB VRAM) mining GPUs on a Chinese BTC79X5 / X79 (Intel H61 chipset) board with
Resizable BAR, enlarged MMIO, and GPU-to-GPU BAR1 P2P.

Everything here is the result of hands-on testing on real hardware. Use at your own
risk — flashing a BIOS can brick a board. Always back up your current flash first.

**Start here for a clean-room reproduction: [docs/SETUP-ubuntu-24.04.md](docs/SETUP-ubuntu-24.04.md)**
— the complete step-by-step recipe (firmware → Ubuntu → driver → Gen2 → P2P verification),
validated 2026-09-16.

## Hardware

- Board: BTC79X5 / X79ETH03 (Intel H61 chipset, single LGA2011 Xeon E5).
- GPUs: NVIDIA CMP 50HX (TU102, `10de:1e09`), 20 GiB VRAM each, 2–4 cards.
- CPU: Xeon E5 (Sandy/Ivy Bridge-E), all cores unlocked (HT/Turbo).
- Firmware base: AMI Aptio, BIOS 4.6.5.

## What this achieves

| Capability | How |
|---|---|
| 16 GiB BAR1 per card | `rebar-xve-tu102-v2.rom` (XVE pre-pass DXE driver) |
| 32 GiB BAR1 per card | same firmware + `XveBar1Selector=9` |
| 128 GiB MMIOH | Setup EFI var (no reflash) — fits 2× 32 GiB |
| 256 GiB MMIOH | 1-byte code patch (`rebar-xve-tu102-v2-mmioh256g.rom`) — fits 4× 32 GiB; **what the production host runs** |
| PCIe Gen2 x8 | pre-driver rescan (`cmp50hx-gen2-rescan`) + deferred retrain (`cmp50hx-gen2`) |
| BAR1 P2P | aikitoria fork (built-in HAL routing + `p2pOverride=0x11`; no source patches) + static BAR1 + `RMForceStaticBar1=1` + `iommu=pt` — **verified 2.47 GB/s bidirectional, 0 mismatch** |

### Measured workload impact (Qwen3.8-27B)

Synthetic wins don't guarantee inference wins. On the actual Qwen3.8-27B workload, BAR1
64 MiB → 32 GiB + P2P off → on moved decode by ~1 % and prefill by ~1–3 % — decode is
compute-bound on the CMP 50HX (TU102, no tensor cores), so the PCIe link was never the
bottleneck. Full before/after benchmark and methodology: [docs/BENCH-QWEN38.md](docs/BENCH-QWEN38.md).

## The path from scratch

The full recipe is [docs/SETUP-ubuntu-24.04.md](docs/SETUP-ubuntu-24.04.md). Short version:

### 1. Flash the firmware (32 GiB BAR1 + 256 GiB MMIOH)

`firmware/rebar-xve-tu102-v2-mmioh256g.rom` is the full 8 MiB image with the XVE pre-pass
(`firmware/XveTu102.c`), which unlocks the card's XVE block and programs a Resizable BAR1
before `PciBusDxe` parses the BARs, plus the 256 GiB MMIOH patch. Flash it, set
`XveBar1Selector=9` (32 GiB), then verify:

    lspci -vvv -s 01:00.0 | grep 'Region 1'   # size=32G

`XveBar1Selector` is one global EFI variable (all cards share the same BAR1 size):
selector `8` = 16 GiB, `9` = 32 GiB. Plan the selector against the eventual card count
BEFORE adding cards — a card that doesn't fit the MMIOH pool re-hangs POST.

### 2. Ubuntu + kernel cmdline

`pci=realloc=on intel_iommu=on iommu=pt modprobe.blacklist=nvidia,nvidia_drm,nvidia_modeset,nvidia_uvm`

### 3. Driver (aikitoria fork, no source patches)

Build/install the `aikitoria/open-gpu-kernel-modules` branch `610.43.03-p2p`
(`./install.sh`), userspace from the stock NVIDIA 610.43.03 `.run --no-kernel-modules`.
modprobe: `NVreg_EnablePCIeGen3=1 cmp50_rebar_size=0 NVreg_RegistryDwords="RMForceStaticBar1=1"`.

### 4. Restore Gen2

`scripts/cmp50hx-gen2-rescan.sh` / `.service` (pre-driver remove/rescan, loads `nvidia`
exactly once) + `scripts/cmp50hx-gen2` / `.service` (deferred retrain that waits for the
card's own TLS unlock, then retrains once — this is what actually lands Gen2). Both units
enabled; the pre-driver retrain alone leaves the live link at Gen1.

### 5. BAR1 P2P (docs/bar1-p2p-tu102-force-enable.md)

The aikitoria 610.43.03-p2p fork already carries everything driver-side: it routes the
default (pre-Hopper) HAL to the GH100 BAR1-P2P implementations, defaults
`pcieP2PType=BAR1`, and sets `p2pOverride=0x11` (READ+WRITE enable) in `kernel_bif.c`.
The two extra patches from the bayley/cmpunlocker set are NOT needed on this build:

- **0013 (skip mailbox peer pre-registration) is a compile-time no-op** —
  `gpumgrGetGpuLinkCount` is `#define ... ((NvU32) 0)`, so `_kbusInitP2P_GM107` is
  constant-folded dead and `peerNumberMask` is already clear.
- **0015 (force the read cap) is redundant** — `p2pOverride=0x11` already decodes to
  READ_ENABLE+WRITE_ENABLE, and `_kp2pCapsCheckStatusOverridesForPcie` runs BEFORE
  `_p2pCapsGetHostSystemStatusOverPcieBar1`, so the read cap is already forced and the
  0015 target function is never reached.

The gate is a static BAR1 ≥ client-visible FB (32 GiB for 20 GiB VRAM) — resolved by the
256 GiB MMIOH firmware patch (see above), **verified working 2026-09-16**. The mailbox
fallback corrupts host RAM (it is reached when `pcieP2PType=BAR1` but static BAR1 is
absent, and `iommu=pt` turns the bogus DMA into silent corruption), so enable static BAR1
before loading the fork. Acceptance test: `scripts/p2p_benchmark.c` (async peer DMA
≈90% of H2D reference + integrity check).

## Repository layout

```
firmware/
  rebar-xve-tu102-v2.rom             base firmware (16 GiB BAR1)
  rebar-xve-tu102-v2-mmioh256g.rom   same + 256 GiB MMIOH patch (production)
  XveTu102.c / XveTu102.h            XVE pre-pass DXE driver source
  ReBar.c                            ReBarUEFI resize pass source
  apply_patch.py                     applies the 256 GiB patch (+ FFS checksum)
  PATCH_MMIOH_256G.md                patch documentation
docs/
  SETUP-ubuntu-24.04.md              full clean-room setup recipe (start here)
  rebar-32g-full-vram-enable.md      ReBAR + MMIOH recipe (validated)
  bar1-p2p-tu102-force-enable.md     BAR1 P2P force-enable recipe
  tu102-xve-dxe-prepatch.md          DXE pre-pass internals
  ai100gb-rebar-p2p-hardrules.md     hard-won operational rules
  btc79x5-firmware-policy.md         CPU-unlock firmware policy
  BENCH-QWEN38.md                     real-workload benchmark (before/after, conclusion)
scripts/
  cmp50hx-gen2-rescan.sh             pre-driver rescan + retrain (deployed)
  cmp50hx-gen2-rescan.service
  cmp50hx-gen2                       deferred Gen2 retrain (fail-closed, deployed)
  cmp50hx-gen2.service
  p2p_benchmark.c                    P2P acceptance test (BW + integrity)
  bench-qwen38.py                     llama-server prefill/decode benchmark
```

## Safety

- Always back up your current flash first: `flashrom -p internal -c MX25L6405 -r backup.rom`.
- Patch the live dump (not the clean source) to preserve NVRAM (Setup vars, MRC
  memory-training data, MAC).
- Judge a flash by the final `Verifying flash... VERIFIED`, not a mid-write erase error
  (flashrom falls back to another erase function).
- Keep local power/console access for POST-hang recovery (CMOS/NVRAM clear or SPI reflash).

## Not included (intentionally)

Runtime NVRAM dumps (they contain the board MAC, MRC training data, and ME runtime
state), API keys, tokens, and internal network details. The ROMs here are clean,
rebuilt images without the board's runtime identifiers.
