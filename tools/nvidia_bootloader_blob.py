#!/usr/bin/env python3
"""Inspect NVIDIA 570.144 extracted GSP bootloader wrapper without loading it."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

FIELDS = (
    "version", "bootloaderOffset", "bootloaderSize", "bootloaderParamOffset",
    "bootloaderParamSize", "riscvElfOffset", "riscvElfSize", "appVersion",
    "manifestOffset", "manifestSize", "monitorDataOffset", "monitorDataSize",
    "monitorCodeOffset", "monitorCodeSize", "monitorEnabled", "swbromCodeOffset",
    "swbromCodeSize", "swbromDataOffset", "swbromDataSize", "fbReservedSize",
    "signedAsCode",
)


def parse(blob: bytes) -> dict:
    if len(blob) < 24:
        raise ValueError("short firmware wrapper")
    vendor, wrapper_version, declared_size, desc_off, image_off, image_size = struct.unpack_from("<6I", blob)
    if vendor != 0x10DE or wrapper_version != 1:
        raise ValueError("unexpected wrapper vendor/version")
    if desc_off < 24 or desc_off + 84 > len(blob) or image_off < desc_off + 84:
        raise ValueError("descriptor bounds invalid")
    if image_off > len(blob) or image_size > len(blob) - image_off:
        raise ValueError("image bounds invalid")
    if declared_size < image_size:
        raise ValueError("declared allocation smaller than image")
    values = dict(zip(FIELDS, struct.unpack_from("<21I", blob, desc_off)))
    image = blob[image_off:image_off + image_size]
    for name in ("bootloader", "bootloaderParam", "manifest", "monitorData", "monitorCode"):
        offset, size = values[name + "Offset"], values[name + "Size"]
        if not size or offset > image_size or size > image_size - offset:
            raise ValueError(f"{name} outside image")
    if values["version"] != 5 or values["monitorEnabled"] != 1 or values["fbReservedSize"] != image_size:
        raise ValueError("unexpected AD103 bootloader descriptor")
    return {"wrapperVersion": wrapper_version, "declaredSize": declared_size,
            "descriptorOffset": desc_off, "imageOffset": image_off,
            "imageSize": image_size, "imageSha256": hashlib.sha256(image).hexdigest(),
            "descriptor": values}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bootloader", type=Path)
    args = parser.parse_args()
    print(json.dumps(parse(args.bootloader.read_bytes()), indent=2))


if __name__ == "__main__":
    main()
