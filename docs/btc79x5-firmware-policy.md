# BTC79X5 firmware policy

## Preserve an existing ROM before considering CPU unlock

1. Extract FFS files from the candidate ROM and the currently proven ROM; compare complete FFS hashes, not whole-image offsets.
2. For BTC79X5/X79ETH03 4.6.5 images, CPU core/HT/Turbo policy is stored in the compressed `NVRAM external defaults` FFS `9221315B-30BB-46B5-813E-1B1BF4712BD3`, not in a CPU PEI or microcode patch.
3. Decode the `Setup` defaults and verify Hyper-Threading, model-specific Active Processor Cores, and Turbo values. If that FFS already matches a known-good CPU-unlock image, do not flash a no-op ROM.
4. Preserve the XVE/ReBAR DXE FFS, flash descriptor, ME region, and NVAR volumes as separate invariants; a flat binary overlay from another BIOS is unsafe even when board names and version strings match.

## BIOS defaults versus runtime policy

- `Above 4G Decoding` is `Setup[0x01]` (`0=disabled`, `1=enabled`) and `Launch CSM` is `Setup[0x15f]` (`1=enabled`, `0=disabled`) in the verified 4.6.5 Setup varstore `EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9`.
- Treat `Setup` factory defaults, live NVRAM variables, IFR defaults, and external defaults as distinct stores. Change all proven sources coherently or leave factory defaults alone and configure Setup manually; changing one source can be discarded during AMI recovery/default selection.
- Keep the XVE BAR selector as a recoverable EFI variable. Do not bake a large BAR selector into NVAR defaults: after a flash or a failed POST, the ability to boot with a display adapter and clear or lower that selector is the recovery path.
- Above 4G/CSM are firmware settings. CMP unlock and Gen2 training are Linux driver/service policies; never claim they persist as BIOS defaults.

## Verify the live result

Confirm CPU topology with `lscpu`, Turbo with `intel_pstate/no_turbo`, Setup bytes through efivars, BAR sizes through `lspci`, and link generation through `nvidia-smi`. An enabled systemd unit is not proof of Gen2: require a live 5.0 GT/s reading on each CMP.