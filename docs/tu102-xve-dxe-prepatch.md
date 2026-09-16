# TU102 CMP 50HX: firmware DXE pre-pass that really yields a 16 GiB BAR1

## Forensic correction: read before using historical conclusions

- Treat selector-9 lost connectivity as an unresolved boot failure, not proof of POST hang, BAR2/MMU failure, or universal impossibility. Require a same-boot POST/serial/allocator trace; OS-side resize failures are a separate experiment.
- Check actual MMIOH resource budget, not just the Linux root resource envelope. BTC79X5 v2 IFR offers MMIOH Size at Setup[0xD9]: 64G=0x40 default and 128G=0x80; the stored v2 Setup payload has 0x40. Observed allocations occupy the 64-GiB high interval 0x380000000000–0x380fffffffff. Two 32-GiB BAR1s plus two 32-MiB BAR3s exceed that interval before bridge-alignment overhead. This is a strong hypothesis, not a verified failed-boot cause or permission to alter Setup. The larger root envelope is about 56 TiB, not the historical 3.7-TB figure below, and is not proof of allocatable GCD/IIO capacity. Read the LIVE value from efivars instead of trusting the ROM default, and run the one-card selector-9 test before any two-card retry — offsets, capacity table and the discriminating test are in `references/<host>-rebar-p2p-hardrules.md` §2.
- Decode Type-1 temporary bridge windows in regression tests: address bits 31:20 map to field bits 15:4. v2 XveTu102.c:688 incorrectly uses Start & 0xFFF0, opening 0..0xffffffff for a 16-MiB scratch. A minimal offline-tested correction exists, but affects selectors 8 and 9 alike and is not a proven 32-GiB boot fix.
- Audit all size writers and raw BAR layouts. Generic ReBarDxe can overwrite XVE's size after its pcie-ok log; v3's BDF gate only gates XVE. Actual CMP BAR0 is 32-bit, so v3's unconditional pairing of config DWORDs as three 64-bit BARs mislabels diagnostics. Modern edk2 allocator fallback is not a guarantee about the proprietary AMI PciBus in the ROM.
- Isolate EDK2 environment as well as sources: explicitly set WORKSPACE, EDK_TOOLS_PATH, CONF_PATH, and PATH before sourcing edksetup.sh. Inherited CONF_PATH can rewrite shared Conf/BuildEnv.sh even when output and sources are copied.

Evidence, source citations, RED/GREEN extracted-function tests, and non-deployable EFI: `/home/fastchip/cmp50-32g-fix/firmware/REPORT.md`. No patched ROM was produced. Hardware validation of selector9 remains missing. Interpret contrary categorical statements later in this historical reference in light of this correction.

Measured working on <host> (BTC79X5 / X79ETH03, AMI Aptio V, 2x CMP 50HX `10de:1e09`,
Ubuntu 24.04 / 6.8) on 2026-09-15. Result: `lspci` `Region 1: [size=16G]`,
`BAR 1: current size: 16GB, supported: 64MB … 16GB`, `nvidia-smi -q` `BAR1 Memory Total: 16384 MiB`,
driver 610.43.03 initializes, production llama.cpp service serves on both GPUs.
Artifacts: `/ai/cmp/bios/rebar-xve-tu102-v2.rom` (sha256 `34d0d479…`), report
`/ai/cmp/bios/rebar-xve-tu102-v2.report.md`, sources/diff in `/ai/cmp/bios/build-v2/`.

## Two facts that decide the whole approach

1. **Flashing a full ROM image rewrites the NVAR volume** (`0x400000-0x420000` on this board).
   A module that is only needed on the first boot after a flash therefore must NOT be gated on an
   EFI variable — the variable will be gone. Observed failure: a ReBarUEFI build whose hook install and
   pre-pass both sat inside `if (reBarState)`, flashed with the variable store of the base image, did
   nothing at all and produced no log — which looks exactly like "the module was never dispatched".
   Use a safe *active* default (smallest step above stock: BAR1 128 MiB) and let a variable only select
   a bigger target. Do not assume a CMOS reset clears this UEFI variable: recover a POST wedge by booting
   without the CMP cards (using a display adapter), then change or delete the selector through efivars.
2. **PciBusDxe calls the hook before it parses BARs.** `PreprocessController(...,
   EfiPciBeforeResourceCollection)` is issued in `PciDetectDevice()` before `PciSearchDevice()` →
   `PciParseBar()` (`MdeModulePkg/Bus/Pci/PciBusDxe/PciEnumeratorSupport.c:290`). So programming the
   PCIe Resizable-BAR control register of BAR1 inside that hook IS what the firmware then allocates.
   Verified: same image, selector 1 → firmware allocated a 128 MiB, 128 MiB-aligned BAR1; selector 8 →
   16 GiB, 16-GiB-aligned (`0x380800000000`, `0x380000000000`, exactly 16 GiB apart).

## Procedure

1. Base: the ROM currently in flash (back it up with `flashrom -r` first). Inserting one new FFS into the
   DXE volume's free space is dispatched normally by this AMI Aptio V build — proven by the module's own
   unconditional entry log. The earlier hypothesis "a file appended in free space is not dispatched /
   needs a DXE_DEPEX section" is wrong on this board: no depex section is required because the driver
   installs its hook via `RegisterProtocolNotify` when `gEfiPciHostBridgeResourceAllocationProtocolGuid`
   is not up yet at entry time.
2. Module entry point (runs once, may run before the variable services are ready — treat variable
   writes there as best-effort):
   - log unconditionally to a byte-array NV variable (existence of that variable = "module was
     dispatched", which no other signal can prove without a console);
   - install the PciHostBridgeResourceAllocationProtocol `PreprocessController` hook unconditionally.
3. Hook body, for every enumerated function with VID/DID `10de:1e09` (once per boot per device):
   - write the XVE block through BAR0: `BAR0+0x88724 <- 0x30` (CYA unlock; this register does not read
     back as written — do not use its readback as success criterion), `BAR0+0x88DCC <- BIT31 | selector`
     (`selector n` ⇒ BAR1 = 2^(n+6) MiB, so 8 = 16 GiB); success signal = `BAR0+0x88BBC` mask grows from
     `0x400` (64 MiB) to `0x7FC00` for selector 8 (bits (selector+10)..), and `lspci` starts listing the
     sizes. Signature-check `0x88BBC` before writing (nonzero, not all-ones, low 10 bits clear, BIT10 set)
     and roll back on failure.
   - BAR0 is usually not assigned at that phase: temporarily point BAR0 at a scratch aperture (top-down
     `gDS->AllocateMemorySpace`, or the top of the 32-bit PCI window), widen the upstream bridges'
     memory windows, do the writes, restore every register. Only the 32-bit bridge-window encoding is easy
     to manipulate, so a scratch address below 4 GiB keeps this simple.
   - then program BAR1's ReBAR control register to `selector+6` (read back; must equal what was written)
     so the size does not depend on ReBarState or on the generic ReBarUEFI resize pass.
4. ReBAR extended-capability offsets (kernel/pciutils convention, easy to get wrong):
   `pci_rebar_find_pos`-style walk returns `Pos = eCapBase + 8*entryIndex`;
   **supported-sizes dword = `Pos + 4` (PCI_REBAR_CAP), control dword = `Pos + 8` (PCI_REBAR_CTRL)`.
   On this GPU the BAR1 entry is at `0xBB8` (so `0xBBC` = supported sizes, `0xBC0` = control); reading
   `eCapBase+4` instead returns BAR0's mask (e.g. `0x10` = 16 MiB) and the size check silently fails.
   Supported-sizes bitmap bit n = 2^(n+20) bytes, control field = n; cross-check both against `lspci -vv`.
5. Packaging: edk2 master built with `-t GCC` (`GCC5` no longer exists), `GenSec` + `GenFfs`, then fix the
   FFS state byte to `0xF8` for a volume with `ERASE_POLARITY=1` (`GenFfs` writes `0x07`, which makes the
   file invisible to the DXE core). Insert with a tool that byte-diffs the result and refuses changes
   outside the inserted range. Build twice and require identical bytes.
6. Flash and verify: `flashrom -r` backup → `flashrom -w` → expect `Erase/write done` +
   `Verifying flash... VERIFIED` → read back and compare → reboot → then (and only then) write the size
   variable, because flashing resets the variable store. Verify with `lspci -vv` (Region size +
   Resizable BAR current/supported), `/proc/iomem` windows, `nvidia-smi -q | grep -A3 'BAR1 Memory'`
   (must read `16384 MiB`, not just the cap), service health, and a real inference request.
7. After any boot, the SPI content will differ from the image in the NVAR area *and* at fixed offsets
   inside the ME region (`0x10069`, `0x1807e`, `0x1c011`, `0x20011`, `0x24002`, `0x24770-0x2CC26` on this
   board) — the ME writes its own areas at runtime, and the same offsets differ for stock and modded
   images. Check "nothing else changed" on the *image* (descriptor `0x0-0xFFF`, ME `0x1000-0x3FFFFF`,
   all pre-existing FV files), not on a post-boot readback in those areas.

## Board/host facts worth reusing

- BIOS already reserved 16 GiB of 64-bit MMIO per BAR1 even while the card advertised 64 MiB; the root
  bridge window above 4 GiB is `0x240000000-0x380fffffffff` (~3.7 TB), so two 16 GiB BARs fit without
  conflict. Alignment is 16 GiB, so the firmware must find 16-GiB-aligned slots (it does).
- If allocation fails, EDK2 `PciBusDxe` shrinks the BAR to its minimum on conflict
  (`AdjustPciDeviceBarSize` → `PciResizableBarMin`), i.e. the failure mode is degradation, and the size
  variable can be reverted remotely without reflashing.
- The custom 610.43.03 module on <host> carries the kernel-side XVE path (`cmp50_rebar_size`, default
  patched to 0 = off) plus CMP50 Gen2/compute-unlock patches; with the firmware doing the XVE work the
  kernel-side path is not needed and BAR1 reaches 16 GiB with the module param at 0.
- Vendor BAR1-SKU claims ("64 MiB until XVE unlock") are about the *advertised* mask; the firmware-side
  write is what turns it into a real aperture, and only `nvidia-smi`'s BAR1 total proves the mapping.
- A kernel-side resize (XVE write from the driver, `setpci` on the ReBAR control, then reassign) does
  produce a big BAR1 — assign it via `remove` + `rescan` of the **upstream root port**, not the device
  (device-only rescan returns `BAR 1 ... can't assign; no space`) — but RM init then dies in its own
  BAR2 self-test (`kbusVerifyBar2`: `MMUTest BAR0 window offset ... returned garbage 0x0` →
  `RmInitAdapter failed (0x24:0x72:1281)`). Do not spend time on kernel-side BAR1 enlargement for these
  cards: only the firmware path yields an aperture that RM accepts.

## P2P on TU102: which mode engages, and the hazard that corrupts host RAM

- Size gate (code fact): `kbusIsStaticBar1Supported_TU102` with `RMForceStaticBar1=ENABLE` requires
  `bar1VASize >= client-visible FB size`, not physical VRAM. The stock 20 GiB CMP therefore needs a
  32 GiB BAR1, which is not POST-safe for two cards on this board. The RM DWORD `OverrideFbSize=16384`
  is documented as reducing FB for memory-management testing; its TU102 path reserves the upper range
  and is a source-backed candidate to make client-visible FB fit a 16 GiB BAR. Treat that route as a
  guarded experiment until `nvidia-smi` reports the reduced memory and the static-BAR1 marker is verified;
  otherwise 16 GiB fails the gate (`NV_ERR_INVALID_REGISTRY_KEY`) and the driver can fall back to mailbox.
- Registry levers: `RMPcieP2PType` (0=MAILBOX, 1=BAR1, 2=AUTO) and `RMForceP2PType`; the aikitoria
  `610.43.03-p2p` fork patches the *default* `pcieP2PType` to BAR1 (`kernel_bif.c`), so state the lever
  explicitly to keep a run reproducible.
- `nvidia-smi topo -p2p r` = OK plus a ~2x peer-vs-staged win (`p2p_test`: 3.3 vs 1.6 GB/s on a Gen2 x8
  link) does **not** prove the path is safe — the mailbox fallback passes capability and bandwidth tests
  and then corrupts host RAM.
- Hazard, measured with the mailbox path engaged under `iommu=pt`: dmesg asserts
  `remoteWMBoxLocalAddr != ~0ULL @ kern_bus_gm200.c:89` and `((base & RM_PAGE_MASK) == 0) @ kern_bus.c:298`,
  and minutes later unrelated processes crash — llama-server, python3, sshd and **systemd[1]** (GPF in
  libc, corrupted journal, `systemctl` stops answering). `iommu=pt` is exactly what converts a bogus
  device write into silent RAM corruption instead of a fault.
- Rules: treat "P2P capability OK" as unproven until a static-BAR1 run is clean; watch dmesg live during
  every P2P test and on the first assert stop, `rollback_driver.sh`, drop `iommu=pt`, reboot; if the system
  is already corrupted use `sysrq-b` (`echo b > /proc/sysrq-trigger`) because systemd-based shutdown will
  hang.

## Reboot and module-reload traps on this host

- The oneshot `cmp50hx-gen2-rescan` service (remove/rescan + retrain, `TimeoutStartSec=600`) hangs
  shutdown while it runs: the machine then sits in halt for 15-20 minutes before coming back. Always
  `systemctl stop` + `disable` it before a reboot, `enable --now` after, and wait for `is-active = active`
  before testing GPUs or link state.
- After a failed RM init the `nvidia` module can stick in `Unloading` (`lsmod` refcount `-1`); `modprobe`
  then fails with `Device or resource busy` and no reload is possible — reboot instead of retrying.
- `flashrom` on this board matches several Macronix variants of the same chip and refuses to run without
  `-c` (`-c MX25L6405D` works). Before writing: confirm the board identity (ROM strings + exactly 8 MiB)
  against the current dump and keep the dump as the rollback path.
- Userspace XVE writes only stick while the card is uninitialized; after GSP has booted the protect-lock
  makes them a no-op, and any reboot clears both the XVE selector and the ReBAR control register unless
  the firmware pre-pass re-applies them.
