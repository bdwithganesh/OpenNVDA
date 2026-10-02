#!/usr/bin/env python3
"""Inspect a local NVIDIA GSP-RM ELF without loading it on a GPU.

Accepts ELF64 little-endian images. Reports each section's bounds and digest so a
firmware port can pin the exact .fwimage and signature it is parsing.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def sections(data: bytes) -> list[dict]:
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4:6] != b"\x02\x01":
        raise ValueError("expected little-endian ELF64")
    header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    machine, shoff, shentsize, shnum, shstrndx = header[1], header[5], header[10], header[11], header[12]
    if machine != 243:
        raise ValueError(f"expected RISC-V machine 243, got {machine}")
    if shentsize != 64 or not shnum or shstrndx >= shnum:
        raise ValueError("invalid section table metadata")
    if shoff > len(data) or shnum > (len(data) - shoff) // shentsize:
        raise ValueError("section table exceeds file")

    entries = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize)
               for i in range(shnum)]
    names = entries[shstrndx]
    if names[4] > len(data) or names[5] > len(data) - names[4]:
        raise ValueError("section name table exceeds file")
    strings = data[names[4]:names[4] + names[5]]

    result = []
    for index, entry in enumerate(entries):
        name_offset, section_type, _, _, offset, size, _, _, _, _ = entry
        if name_offset >= len(strings):
            raise ValueError(f"section {index} has invalid name offset")
        end = strings.find(b"\0", name_offset)
        if end < 0:
            raise ValueError(f"section {index} has unterminated name")
        name = strings[name_offset:end].decode("ascii", errors="replace")
        if section_type != 8 and (offset > len(data) or size > len(data) - offset):
            raise ValueError(f"section {name} exceeds file")
        payload = b"" if section_type == 8 else data[offset:offset + size]
        result.append({"name": name, "type": section_type, "offset": offset,
                       "size": size, "sha256": hashlib.sha256(payload).hexdigest()})
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    args = parser.parse_args()
    data = args.firmware.read_bytes()
    entries = sections(data)
    names = {entry["name"] for entry in entries}
    required = {".fwimage", ".fwsignature_ad10x"}
    if not required <= names:
        raise SystemExit(f"missing required sections: {sorted(required - names)}")
    print(json.dumps({"file": str(args.firmware), "size": len(data),
                      "sha256": hashlib.sha256(data).hexdigest(), "sections": entries}, indent=2))


if __name__ == "__main__":
    main()
