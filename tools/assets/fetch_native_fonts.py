#!/usr/bin/env python3
"""Fetch pinned Fusion Pixel native BDF strikes and their complete notices."""
import argparse
import hashlib
import io
from pathlib import Path
from urllib.request import urlopen
import zipfile

VERSION = "2026.10.25"
ARCHIVES = {
    8: "9bf82cfe4c73accca0a894b28e55b1bdbd2f98ee9e9045d5b6549a632df7f264",
    10: "d204894d684a0a9c016d6a0fb959184e4f3bd4e728917f37aabc256a8de63e61",
    12: "d8788cf5ffd6a2dabec397c33c878261195fb9146cdb67d8afd418fe73db15bd",
}


def fetch(output):
    output.mkdir(parents=True, exist_ok=True)
    for size, expected in ARCHIVES.items():
        name = f"fusion-pixel-font-{size}px-monospaced-bdf-v{VERSION}.zip"
        path = output/name
        data = path.read_bytes() if path.exists() else urlopen(
            f"https://github.com/TakWolf/fusion-pixel-font/releases/download/{VERSION}/{name}",
            timeout=60).read()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"Fusion Pixel archive identity mismatch: {name}")
        if not path.exists():
            path.write_bytes(data)
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            bdf = f"fusion-pixel-{size}px-monospaced-zh_hans.bdf"
            target = output/str(size)/bdf
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(archive.read(bdf))
            for member in archive.namelist():
                part = Path(member)
                if part.is_absolute() or ".." in part.parts:
                    raise ValueError("invalid font archive member")
                if member.endswith("/") or member.lower().endswith((".bdf", ".ttf", ".otf", ".woff", ".woff2")):
                    continue
                # Keep root notices and all upstream component licences.
                destination = output/"licenses"/str(size)/member
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(archive.read(member))
        print(f"Verified native {size}px strike and notices: {target}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    fetch(parser.parse_args().output)
