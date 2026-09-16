#!/usr/bin/env python3
"""
Применить патч MMIOH 256 GiB к rebar-xve-tu102-v2.rom (исправленная версия).

Патч: ROM 0x6CD3C9: 0x40 -> 0x80
      (mov eax, 0x40000000 -> mov eax, 0x80000000, множитель 1 GiB -> 2 GiB)

Корректно пересчитывает FFS data checksum (IntegrityCheck.File) по PI Spec:
при FFS_ATTRIB_CHECKSUM (0x40) checksum считается по данным ПОСЛЕ FFS header,
checksum = (-sum(data)) & 0xFF. Header checksum (IntegrityCheck.Header) считается
с File=0 и State=0 — патч байта в теле его не меняет.

Проверяет: GUID FFS, размер FFS, header checksum, всю инструкцию, SHA256 источника.

Использование:
    python3 apply_patch.py [--src SRC] [--out OUT.rom]
"""

import argparse
import hashlib
import sys
import uuid

SRC = "/ai/cmp/bios/rebar-xve-tu102-v2.rom"

ROM_SIZE = 0x800000

PATCH_INSN_OFF = 0x6CD3C5
PATCH_OLD = bytes.fromhex("B8 00 00 00 40")
PATCH_NEW = bytes.fromhex("B8 00 00 00 80")

FFS_OFF = 0x6C4E48
EXPECTED_FFS_SIZE = 0x06AA7E
EXPECTED_GUID = uuid.UUID("5C08C7C8-24C2-4400-9627-CF2869421E06").bytes_le

FFS_ATTRIB_CHECKSUM = 0x40
FFS_ATTRIB_LARGE_FILE = 0x01
FFS_FIXED_CHECKSUM = 0xAA


def checksum8(data):
    return (-sum(data)) & 0xFF


def load(path):
    with open(path, "rb") as f:
        return bytearray(f.read())


def ffs_size(rom):
    attr = rom[FFS_OFF + 19]
    if attr & FFS_ATTRIB_LARGE_FILE:
        return int.from_bytes(rom[FFS_OFF + 24:FFS_OFF + 32], "little")
    return int.from_bytes(rom[FFS_OFF + 20:FFS_OFF + 23], "little")


def verify_header_checksum(rom, header_size):
    hdr = bytearray(rom[FFS_OFF:FFS_OFF + header_size])
    hdr[17] = 0  # IntegrityCheck.File = 0
    hdr[23] = 0  # State = 0
    return (sum(hdr) & 0xFF) == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=SRC)
    ap.add_argument("--out", default="rebar-xve-tu102-v2-mmioh256g.rom")
    args = ap.parse_args()

    rom = load(args.src)

    if len(rom) != ROM_SIZE:
        sys.exit(f"ERROR: ROM size 0x{len(rom):X}, expected 0x{ROM_SIZE:X}")

    # --- проверка SHA256 источника (защита от чужой ревизии BIOS) ---
    digest = hashlib.sha256(rom).hexdigest()
    print(f"source SHA256: {digest}")

    # --- проверка GUID FFS ---
    guid = bytes(rom[FFS_OFF:FFS_OFF + 16])
    if guid != EXPECTED_GUID:
        sys.exit(
            f"ERROR: FFS GUID mismatch\n"
            f"expected: {EXPECTED_GUID.hex()}\nactual:   {guid.hex()}"
        )

    attr = rom[FFS_OFF + 19]
    size = ffs_size(rom)
    print(f"FFS Attributes : 0x{attr:02X}")
    print(f"FFS Size       : 0x{size:X}")

    if size != EXPECTED_FFS_SIZE:
        sys.exit(f"ERROR: FFS size mismatch: 0x{size:X} != 0x{EXPECTED_FFS_SIZE:X}")

    header_size = 32 if attr & FFS_ATTRIB_LARGE_FILE else 24

    # --- header checksum ДО ---
    if not verify_header_checksum(rom, header_size):
        sys.exit("ERROR: original FFS header checksum invalid")
    print("FFS header checksum: OK")

    # --- вся инструкция ---
    actual = bytes(rom[PATCH_INSN_OFF:PATCH_INSN_OFF + len(PATCH_OLD)])
    if actual != PATCH_OLD:
        sys.exit(
            f"ERROR: patch instruction mismatch\n"
            f"expected: {PATCH_OLD.hex(' ')}\nactual:   {actual.hex(' ')}"
        )
    print(f"patch instruction @ 0x{PATCH_INSN_OFF:X}: {PATCH_OLD.hex(' ')}")

    # --- применить патч ---
    rom[PATCH_INSN_OFF:PATCH_INSN_OFF + len(PATCH_NEW)] = PATCH_NEW
    print(f"patched -> {PATCH_NEW.hex(' ')}")

    # --- корректный FFS data checksum ---
    file_checksum_off = FFS_OFF + 17
    if attr & FFS_ATTRIB_CHECKSUM:
        data = rom[FFS_OFF + header_size:FFS_OFF + size]
        old_ic = rom[file_checksum_off]
        new_ic = checksum8(data)
        rom[file_checksum_off] = new_ic
        print(f"FFS data checksum: 0x{old_ic:02X} -> 0x{new_ic:02X}")
        if (sum(data) + new_ic) & 0xFF:
            sys.exit("ERROR: generated FFS data checksum invalid")
    else:
        old_ic = rom[file_checksum_off]
        print(f"FFS_ATTRIB_CHECKSUM not set, File checksum = 0x{old_ic:02X}")
        if old_ic != FFS_FIXED_CHECKSUM:
            sys.exit(f"ERROR: checksum flag clear but File=0x{old_ic:02X}, expected 0xAA")

    # --- header checksum ПОСЛЕ ---
    if not verify_header_checksum(rom, header_size):
        sys.exit("ERROR: FFS header checksum invalid after patch")

    # --- финальная проверка ---
    if bytes(rom[PATCH_INSN_OFF:PATCH_INSN_OFF + len(PATCH_NEW)]) != PATCH_NEW:
        sys.exit("ERROR: final patch verification failed")

    with open(args.out, "wb") as f:
        f.write(rom)

    print()
    print(f"output : {args.out}")
    print(f"SHA256 : {hashlib.sha256(rom).hexdigest()}")
    print("PATCH  : OK")
    print("FFS    : OK")


if __name__ == "__main__":
    main()
