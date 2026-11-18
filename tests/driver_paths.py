"""Resolve driver directories after the NVIDIA and non-NVIDIA moves.

The NVIDIA stack lives under `drivers/<name>`;
non-NVIDIA drivers live under `drivers/non-nvidia/<category>/<Kext>/`.
Tests should call `driver_dir(name)` instead of hard-coding either layout.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / "drivers"
NVIDIA = DRIVERS / "nvidia-metal-drivers-macos"


def driver_dir(name: str) -> Path:
    candidates = [NVIDIA / name, DRIVERS / name]
    base = DRIVERS / "non-nvidia"
    if base.is_dir():
        candidates.append(base / name)
        candidates.extend(sorted(p for p in base.glob(f"*/{name}") if p.is_dir()))
    for path in candidates:
        if path.is_dir():
            return path
    raise FileNotFoundError(f"driver directory not found: {name}")


def all_driver_dirs():
    """Every directory that looks like a kext (has Info.plist), including the
    categorized non-NVIDIA tree and the NVIDIA tree."""
    seen = set()
    for path in DRIVERS.rglob("Info.plist"):
        d = path.parent
        if d in seen:
            continue
        seen.add(d)
        yield d
