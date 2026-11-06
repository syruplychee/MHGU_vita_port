#!/usr/bin/env python3
"""Fetch the pinned public Unifont input and its OFL attribution."""
import argparse
import gzip
import hashlib
from pathlib import Path
from urllib.request import urlopen

from build_pixel_fonts import UNIFONT_SHA

BASE = "https://unifoundry.com/pub/unifont/unifont-18.0.01/font-builds/"
BDF_SHA = "8c3c5ce5fb524eec5bacbc91b5ddfde81641e2cfc015521e95107b77570368e3"
OFL_SHA = "869692af094c57fb7258c57fe26820c759319603321d0ffeb278de3651763ded"


def fetch(directory, name, url, expected, *, compressed_identity=False):
    path = directory/name
    data = path.read_bytes() if path.exists() else urlopen(url,timeout=60).read()
    identity = gzip.decompress(data) if compressed_identity else data
    if hashlib.sha256(identity).hexdigest() != expected:
        raise ValueError(f"input identity mismatch: {name}")
    if not path.exists():
        path.write_bytes(data)
    return identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output",type=Path,required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    fetch(args.output,"unifont-18.0.01.hex.gz",BASE+"unifont-18.0.01.hex.gz",UNIFONT_SHA)
    fetch(args.output,"OFL-1.1.txt","https://unifoundry.com/OFL-1.1.txt",OFL_SHA)
    bdf = fetch(args.output,"unifont-18.0.01.bdf.gz",BASE+"unifont-18.0.01.bdf.gz",BDF_SHA,compressed_identity=True)
    properties = bdf.decode("utf-8").split("ENDPROPERTIES")[0]
    notice = "\n".join(line for line in properties.splitlines() if line.startswith("COPYRIGHT "))
    if not notice:
        raise ValueError("font copyright property is absent")
    notice += "\nDerived masks: MHGU Vita Pixel. Bitmap atlas generation and sampling strategy are recorded in the package manifest.\n"
    (args.output/"COPYRIGHT.txt").write_text(notice,encoding="utf-8")
    print(f"Pinned Unifont input and OFL attribution verified: {args.output}")


if __name__ == "__main__":
    main()
