# CMP 50HX 20GB × N on BTC79X5 (X79) — ReBAR / MMIOH / P2P

Verified, working firmware mods and scripts for running multiple NVIDIA CMP 50HX
(20 GiB VRAM) mining GPUs on a Chinese BTC79X5 / X79 (Intel H61 chipset) board with
Resizable BAR, enlarged MMIO, and (optionally) GPU-to-GPU BAR1 P2P.

Everything here is the result of hands-on testing on real hardware. Use at your own
risk — flashing a BIOS can brick a board. Always back up your current flash first.

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
| 256 GiB MMIOH | 1-byte code patch — fits 4× 32 GiB (224 GiB) |
| PCIe Gen2 x8 | deferred retrain service (`cmp50hx-gen2-after-gsp`) |
| BAR1 P2P | aikitoria fork (built-in HAL routing + `p2pOverride=0x11`; no source patches) + static BAR1 + `iommu=pt` |

## The path from scratch

Detailed recipes live in `docs/` (in Russian). Short version:

### 1. Flash the base firmware (16 GiB BAR1)

`firmware/rebar-xve-tu102-v2.rom` is the full 8 MiB image with the XVE pre-pass
(`firmware/XveTu102.c`), which unlocks the card's XVE block and programs a 16 GiB
Resizable BAR1 before `PciBusDxe` parses the BARs. Flash it, then verify:

    lspci -vvv -s 01:00.0 | grep 'Region 1'   # size=16G

### 2. Enlarge MMIOH (see docs/rebar-32g-full-vram-enable.md)

- **128 GiB** (no reflash, fits 2× 32 GiB): write the AMI Setup EFI variable —
  MMIOH Size `0xD9 = 0x80`, IOH Resource Selection `0xE2 = 0x01` (Manual).
- **256 GiB** (fits 4× 32 GiB): apply the 1-byte multiplier patch
  (`firmware/apply_patch.py` + `firmware/PATCH_MMIOH_256G.md`). The Intel RefCode
  scales `mmiohSize × 0x40000000` (1 GiB); patch the immediate to `0x80000000` so
  Setup value `0x80` yields 256 GiB. Requires recomputing the FFS data checksum.

### 3. Set BAR1 size (selector)

`XveBar1Selector` is one global EFI variable (all cards share the same BAR1 size):
selector `8` = 16 GiB, `9` = 32 GiB. Plan the selector against the eventual card
count BEFORE adding cards — a 3rd card at selector 9 under a 128 GiB pool re-hangs POST.

### 4. Restore Gen2

`scripts/cmp50hx-gen2-after-gsp.sh` / `.service` do a bounded PCIe retrain only after
the first GSP boot, without PCI remove/rescan (a rescan after `nvidia` opened a card
breaks the unlock chain). Cards otherwise stay at Gen1 x8.

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

So the ONLY remaining gate is a static BAR1 ≥ client-visible FB (32 GiB for 20 GiB
VRAM) — a firmware/POST problem, not a driver problem. The mailbox fallback corrupts
host RAM (it is reached when `pcieP2PType=BAR1` but static BAR1 is absent, and
`iommu=pt` turns the bogus DMA into silent corruption), so enable static BAR1 before
loading the fork and keep IOMMU translated while testing.

## Repository layout

```
firmware/
  rebar-xve-tu102-v2.rom             base firmware (16 GiB BAR1)
  rebar-xve-tu102-v2-mmioh256g.rom   same + 256 GiB MMIOH patch
  XveTu102.c / XveTu102.h            XVE pre-pass DXE driver source
  ReBar.c                            ReBarUEFI resize pass source
  apply_patch.py                     applies the 256 GiB patch (+ FFS checksum)
  PATCH_MMIOH_256G.md                patch documentation
docs/
  rebar-32g-full-vram-enable.md      full ReBAR + MMIOH recipe (validated)
  bar1-p2p-tu102-force-enable.md     BAR1 P2P force-enable recipe
  tu102-xve-dxe-prepatch.md          DXE pre-pass internals
  ai100gb-rebar-p2p-hardrules.md     hard-won operational rules
  btc79x5-firmware-policy.md         CPU-unlock firmware policy
scripts/
  cmp50hx-gen2-after-gsp.sh          deferred Gen2 retrain (fail-closed)
  cmp50hx-gen2-after-gsp.service
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
