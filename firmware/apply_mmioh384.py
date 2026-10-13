#!/usr/bin/env python3
"""BTC79X5: patch validated 256-GiB MMIOH code to 384 GiB.
Offline only: no flashing. Preserve the input; output must not exist.
For deployment use a fresh local SPI backup, NOT another host's NVRAM.
"""
import argparse
import hashlib
from pathlib import Path
import uuid

def patch(data, reference):
    start = 0x6C4E48
    size = 0x6AA7E
    if len(data) != 8388608 or len(reference) != 8388608:
        raise ValueError('Expected 8 MiB images')
    if data[start:start+16] != uuid.UUID('5C08C7C8-24C2-4400-9627-CF2869421E06').bytes_le:
        raise ValueError('Unexpected PEI module GUID')
    if int.from_bytes(data[start+20:start+23], 'little') != size:
        raise ValueError('Unexpected FFS size')
    if data[start:start+size] != reference[start:start+size]:
        raise ValueError('PEI module differs from validated 256G reference')
    header = bytearray(data[start:start+24])
    header[17] = header[23] = 0
    if sum(header) % 256 or not (data[start+19] & 0x40):
        raise ValueError('Invalid FFS header/checksum attributes')
    if (sum(data[start+24:start+size]) + data[start+17]) % 256:
        raise ValueError('Invalid FFS data checksum')
    if data[0x6CD3C5:0x6CD3CA] != bytes.fromhex('b800000080'):
        raise ValueError('Expected mov eax,0x80000000')
    result = bytearray(data)
    result[0x6CD3C9] = 0xC0
    result[start+17] = (-sum(result[start+24:start+size])) & 255
    diff = [i for i, (a, b) in enumerate(zip(data, result)) if a != b]
    if diff != [start+17, 0x6CD3C9]:
        raise ValueError('Expected exactly multiplier and checksum changes')
    if (sum(result[start+24:start+size]) + result[start+17]) % 256:
        raise ValueError('Output checksum failed')
    return bytes(result)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    ref = Path(__file__).with_name('rebar-xve-tu102-v2-mmioh256g.rom').read_bytes()
    if hashlib.sha256(ref).hexdigest() != '4de9dc245e9bd26b497d8af95445e23fd15fbdf12bc540a2eb2df9a3d79e8165':
        raise ValueError('Unexpected reference image SHA256')
    output = patch(args.input.read_bytes(), ref)
    with args.output.open('xb') as f:
        f.write(output)
    print(hashlib.sha256(output).hexdigest(), args.output)

if __name__ == '__main__':
    main()
