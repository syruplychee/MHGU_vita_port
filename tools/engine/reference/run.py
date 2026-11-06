"""Build synthetic inputs and compare preparation with locked A32 instructions.

Offline analysis only. No interpreter or game image is included in the VPK.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    output = args.output.resolve()
    if output.exists():
        parser.error("Output must be a fresh directory; old cases must not contaminate the gate")
    image = args.image.resolve()
    if hashlib.sha256(image.read_bytes()).hexdigest() != "24c208c395c65727b21b5fa91af8381396e70cf421b0fcfee0345d8743298110":
        parser.error("Locked main image identity mismatch")
    subprocess.run([args.cmake, "-S", str(source), "-B", str(output)], check=True)
    subprocess.run([args.cmake, "--build", str(output), "--config", "Release"], check=True)
    for kind in ("material", "state", "queue"):
        cases = output / kind
        cases.mkdir()
        exe = output / "Release" / f"{kind}_fixture.exe" if sys.platform == "win32" else output / f"{kind}_fixture"
        subprocess.run([str(exe)], cwd=cases, check=True)
        subprocess.run([sys.executable, str(source / f"compare_{kind}.py"),
                        "--image", str(image), "--fixtures", str(cases)], check=True)


if __name__ == "__main__":
    main()
