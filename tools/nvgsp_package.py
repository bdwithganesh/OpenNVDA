#!/usr/bin/env python3
"""Build the pinned 570.144 AD103 GSP staging package."""
import argparse, hashlib, json, struct
from pathlib import Path
from nvidia_gsp_elf import sections
from nvidia_bootloader_blob import parse as parse_bootloader

MAGIC = b"NVGSPKG\0"
TYPES = {"fwimage": 1, "signature": 2, "boot_image": 3,
         "boot_descriptor": 4, "booter_load": 5}

def align(value: int, amount: int = 4096) -> int:
    return (value + amount - 1) & ~(amount - 1)

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gsp", type=Path, required=True)
    ap.add_argument("--bootloader", type=Path, required=True)
    ap.add_argument("--booter-load", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    elf = args.gsp.read_bytes()
    sec = {s["name"]: s for s in sections(elf)}
    boot = args.bootloader.read_bytes()
    parsed = parse_bootloader(boot)
    image_off, image_size = parsed["imageOffset"], parsed["imageSize"]
    payloads = [
        ("fwimage", elf[sec[".fwimage"]["offset"]:][:sec[".fwimage"]["size"]]),
        ("signature", elf[sec[".fwsignature_ad10x"]["offset"]:][:sec[".fwsignature_ad10x"]["size"]]),
        ("boot_image", boot[image_off:image_off + image_size]),
        ("boot_descriptor", boot[parsed["descriptorOffset"]:][:84]),
        ("booter_load", args.booter_load.read_bytes()),
    ]
    header_bytes = 32 + 32 * len(payloads)
    cursor = align(header_bytes)
    entries, manifest = [], []
    for name, data in payloads:
        entries.append((TYPES[name], 0, cursor, len(data), 0))
        manifest.append({"name": name, "type": TYPES[name], "offset": cursor,
                         "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        cursor = align(cursor + len(data))
    out = bytearray(cursor)
    struct.pack_into("<8sIIQQ", out, 0, MAGIC, 1, len(entries), cursor, 0)
    for i, (entry, (_, data)) in enumerate(zip(entries, payloads)):
        struct.pack_into("<IIQQQ", out, 32 + i * 32, *entry)
        out[entry[2]:entry[2] + len(data)] = data
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(out)
    result = {"file": str(args.out), "size": len(out),
              "sha256": hashlib.sha256(out).hexdigest(), "entries": manifest}
    args.out.with_suffix(args.out.suffix + ".json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))

if __name__ == "__main__": main()
