# Full 32 GiB BAR1 (ReBAR) and 256 GiB MMIOH on BTC79X5 + CMP 50HX

Applies to: <host> (<host-ip>), X79/BTC79X5 (Intel H61 chipset), 2–4× CMP 50HX
(TU102, 10de:1e09), firmware `rebar-xve-tu102-v2.rom` (XVE pre-pass). Validated on hardware
2026-09-16 (both 128 GiB and 256 GiB MMIOH).

## Prerequisites

- The XVE pre-pass firmware is already flashed and selector 8 (16 GiB) boots cleanly with
  20 GiB VRAM per card and GSP ready.
- SSH + passwordless sudo (`sudo -n` works) + local power/console access for risky reboots.

## Root cause of the old "selector 9 = 32 GiB hangs POST"

It is NOT a bug in the XVE selector arithmetic. `XveGetSelector` accepts 9 and
`XveSetPciBar1Size` writes Want=15 (`2^15 MiB`), no overflow. The failure is downstream in
MMIO allocation: the MMIOH pool default is **64 GiB** (Setup offset 0xD9 = 0x40).

- 16+16 GiB BAR1 + 2× 32 MiB BAR3 + bridge alignment ≈ 48 GiB → fits in 64 GiB.
- 32+32 GiB BAR1 + BAR3 + alignment ≈ 96 GiB → exceeds 64 GiB → allocator fails → POST hang.

The fix is to enlarge the pool, not to patch the selector code.

## Step 1 — Enlarge MMIOH pool to 128 GiB (remote, no reflash)

Write the AMI Setup EFI variable (GUID `ec87d643-eba4-4bb5-a1e5-3f3e36b20da9`).
Data starts after 4 attribute bytes; data offset N is file offset N+4.

- MMIOH Size @ data offset `0xD9` → `0x80` (128G). File offset 221.
- IOH Resource Selection Type @ data offset `0xE2` → `0x01` (Manual). File offset 230.

Encoding (from IFR): MMIOH 1G=0x01, 2G=0x02, 4G=0x04, 8G=0x08, 16G=0x10, 32G=0x20,
64G=0x40 (default), 128G=0x80. IOH: Auto=0x00 (default), Manual=0x01.

```bash
V=/sys/firmware/efi/efivars/Setup-ec87d643-eba4-4bb5-a1e5-3f3e36b20da9
sudo cat "$V" > /home/fastchip/setup-backup.bin && sha256sum /home/fastchip/setup-backup.bin
cp /home/fastchip/setup-backup.bin /tmp/setup-new.bin
printf '\x80' | dd of=/tmp/setup-new.bin bs=1 seek=221 conv=notrunc   # MMIOH 128G
printf '\x01' | dd of=/tmp/setup-new.bin bs=1 seek=230 conv=notrunc   # IOH Manual
od -An -tx1 -j 221 -N1 /tmp/setup-new.bin   # want 80
od -An -tx1 -j 230 -N1 /tmp/setup-new.bin   # want 01
sudo chattr -i "$V"   # efivarfs marks vars immutable
sudo tee "$V" < /tmp/setup-new.bin > /dev/null
sudo cat "$V" | od -An -tx1 -j 221 -N1   # readback verify
sudo chattr +i "$V"
```

Stop/disable the Gen2 service before ANY reboot (else shutdown halts 15-20 min):
```bash
sudo systemctl stop cmp50hx-gen2-rescan.service
sudo systemctl disable cmp50hx-gen2-rescan.service   # `mask` fails: real unit file
```

## Step 2 — Reboot with selector still 8, confirm the pool moved

Verify 16 GiB still works AND the allocation shifted up ~64 GiB (proof the MMIOH window
grew, not just _CRS): `lspci -vvv -s 01:00.0 | grep 'Region 1'` and
`dmesg | grep 'root bus resource'` (upper limit moved up 64 GiB).

## Step 3 — Set selector 9 (32 GiB), reboot

```bash
V=/sys/firmware/efi/efivars/XveBar1Selector-a3c5b77a-c88f-4a93-bf1c-4a92a32c65ce
sudo chattr -i "$V"
printf '\x07\x00\x00\x00\x09' | sudo tee "$V" > /dev/null
sudo cat "$V" | od -An -tu1   # want 7 0 0 0 9
sudo systemctl reboot
```

## Step 4 — Verify success

`lspci -vvv -s 01:00.0 | grep -E 'Region 1|Region 3'` → Region 1 [size=32G];
`nvidia-smi --query-gpu=name,memory.total --format=csv` → 20480 MiB;
`journalctl -b -k | grep -E 'CMP50_GSP_READY|RmInitAdapter|kbusVerifyBar2|garbage'` →
GSP_READY, zero RmInitAdapter, zero BAR2 garbage.

## Recovery if a step hangs POST

Power off; remove the CMP cards; boot with a display adapter; set selector 8 (or 0) via
efivars, restore the Setup variable from the backup; reinstall cards. CMOS battery jumper
alone may NOT clear the EFI variables (they live in SPI NVRAM, not battery CMOS) — use the
board NVRAM-clear path or an external SPI programmer if a bare CMOS clear does not bring POST back.

## Why firmware-first 32 GiB works where OS-side resize failed

The old BAR2 test failures (`kbusVerifyBar2_GM107 garbage 0x0`, `RmInitAdapter 0x24:0x72`) were
observed only on the OS-side resize route. With the firmware XVE pre-pass + enlarged MMIOH,
GSP boots cleanly at 32 GiB BAR1. Firmware pre-assignment is a separately testable route — do
not conclude the card cannot take 32 GiB from an OS-side resize failure.

## MMIOH capacity ceiling and card scaling

- MMIOH Size is an 8-bit Setup field; the IFR max is `0x80` = 128 GiB (256 GiB = `0x100` does
  not fit in one byte). This caps how many large-BAR cards fit WITHOUT a code patch.
- Each 32 GiB card costs 64 GiB of MMIO (32 GiB BAR1 + 32 GiB bridge-alignment gap); each
  16 GiB card costs 32 GiB.
  - 2×32 = 96 GiB → fits 128. 3×32 = 160, 4×32 = 224 → exceed 128 → POST hang.
  - 4×16 = 112 GiB → fits 128; 5×16 = 144 → exceeds.
- `XveBar1Selector` is one global EFI variable — every card gets the same BAR1 size; there is no
  per-card mixing without a firmware change. Plan the selector against the eventual card count
  BEFORE installing a new card (a 3rd card at selector 9 re-triggers the POST hang).

## 256 GiB MMIOH (for 4×32 GiB = 224 GiB) — validated on hardware

The silicon is NOT the limit: LGA2011 IIO `LMMIOH_BASE`/`LMMIOH_LIMIT` decode address bits
A[50:26], far beyond 128 GiB. The ceiling is the Intel RefCode / Setup. MMIOH size is handled
numerically inside the `IvtQpiandMrcInit` PEI module (GUID `5C08C7C8-24C2-4400-9627-CF2869421E06`,
Intel RefCode / IOH init) — debug strings `mmiohSize: %u GB` / `%08X`, and there are NO
`256G`/`512G`/`1024G`/`LMMIOH` strings in the ROM, so there is no hidden 256G menu option to
simply unhide. There is also NO upper clamp (`cmp ,0x80`) in the code — only a 0→2 GiB lower
guard — so the 128 GiB ceiling is purely the 8-bit Setup field + IFR.

The code scales `mmiohSize × 0x40000000` (1 GiB) as a single instruction
`mov eax, 0x40000000` (bytes `B8 00 00 00 40`) at VMA 0xFFECD3C5 / ROM 0x6CD3C5, inside the
UNCOMPRESSED PE32 (ImageBase 0xFFEC4EA0; FFS ROM 0x6C4E48, size 0x6AA7E). Patch ONE byte to
double the multiplier (1 GiB → 2 GiB), reinterpreting Setup 0x80 as 256 GiB:

- **ROM 0x6CD3C9: `0x40` → `0x80`** (`mov eax,0x40000000` → `mov eax,0x80000000`).
- OFF-BY-ONE: the patched byte is the LAST byte of the little-endian immediate (0x6CD3C9 reads
  0x40); 0x6CD3C8 reads 0x00 and patching it yields 0x40008000 — wrong.
- Doubling the multiplier doubles ALL Setup values (0x40→128 GiB, 0x80→256 GiB) and the
  0→2 GiB guard becomes 4 GiB (unused — we set 0x80).

Validated result: root bus resource upper limit `0x381FFFFFFFFF` (128 GiB) → `0x383FFFFFFFFF`
(256 GiB); 2×32 GiB BAR1 land at `0x382000000000`/`0x383000000000`; clean GSP, no BAR2 error.
4×32 GiB (224 GiB) now fits with ~32 GiB spare.

### FFS data checksum — MUST recompute when patching a byte in a module body

Per PI Specification, `IntegrityCheck.File` (header offset 17) covers the DATA AFTER the FFS
header (not the whole file) when `FFS_ATTRIB_CHECKSUM` (Attributes bit 0x40) is set:
checksum = `(-sum(data)) & 0xFF`. Header checksum (offset 16) is computed with File=0 and
State=0 and is NOT affected by body edits. `EFI_FIRMWARE_VOLUME_HEADER.Checksum` covers only
the FV header, so a body byte edit does not break it. A body patch therefore requires
recomputing ONLY the module's data checksum (a 1-byte body patch becomes a 2-byte diff:
patch byte + data checksum). Verify before flashing: read Attributes (0x40 = checksum set),
confirm `sum(header with File=0,State=0) == 0` and `sum(data)+File == 0`.

### Flash workflow — patch the current flash dump, not the source ROM

Flash from the running OS with `flashrom -p internal` (Intel H61 chipset, Macronix MX25L6405).
Multiple MX25L64xx chip definitions match, so pass `-c MX25L6405` explicitly. Procedure:

1. Backup the LIVE flash (not a copy of the source ROM):
   `sudo flashrom -p internal -c MX25L6405 -r backup-current.rom` (8 MiB) + sha256.
2. Apply the 2-byte patch to the BACKUP, not the clean source ROM — the live flash holds the
   current NVRAM (Setup vars, MRC memory-training data, MAC), and re-flashing the clean source
   resets all of it. First confirm the target module in the backup is byte-identical to the
   source (so the patch offset is valid), then patch + recompute the data checksum.
3. `sudo flashrom -p internal -c MX25L6405 -w patched-current.rom` then
   `-v patched-current.rom`. flashrom's first erase function can fail ("ERASE FAILED"), but it
   falls back to another erase function and recovers — judge by the final "Verifying flash...
   VERIFIED", not the first erase error.
4. Reboot and confirm the aperture grew (root bus resource limit) before trusting it.

## After 32 GiB BAR1 is up

- Restore Gen2 (re-enable the rescan service; cards otherwise stay at Gen1 x8).
- BAR1 P2P path is now unblocked (static BAR1 32 GiB >= 20 GiB FB): see
  `references/bar1-p2p-tu102-force-enable.md` for the driver patches and safety gates.
