"""Read-only verification of the exact MHGU update-1.4.0 executable inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs", required=True, type=Path)
    parser.add_argument("--nacp", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    lock = json.loads((root / "config/target-1.4.0.json").read_text(encoding="utf-8"))
    checks = []

    def check(label, path, expected, kind):
        errors = []
        try:
            data = path.read_bytes()
            if len(data) != expected["size"]:
                errors.append("size mismatch")
            if hashlib.sha256(data).hexdigest() != expected["sha256"]:
                errors.append("SHA256 mismatch")
            if kind == "nso":
                if len(data) < 0x100 or data[:4] != b"NSO0":
                    errors.append("invalid NSO header")
                elif data[0x40:0x60].hex() != expected["build_id"]:
                    errors.append("build ID mismatch")
            elif kind == "npdm":
                if len(data) < 0x80 or data[:4] != b"META":
                    errors.append("invalid NPDM header")
                elif data[0xC] & 1:
                    errors.append("AArch64 executable is unsupported")
            elif kind == "nacp":
                if len(data) < 0x3070:
                    errors.append("truncated NACP")
                else:
                    version = data[0x3060:0x3070].split(b"\0", 1)[0]
                    if version != lock["display_version"].encode("ascii"):
                        errors.append("display version mismatch")
        except OSError as exc:
            errors.append(str(exc))
        checks.append({"name": label, "path": str(path.resolve()),
                       "passed": not errors, "errors": errors})

    for module in lock["modules"]:
        check(module["name"], args.exefs / module["name"], module, "nso")
    # Only the four locked code modules are supported, including future subsdk additions.
    allowed = {module["name"] for module in lock["modules"]} | {"main.npdm"}
    extras = sorted(p.name for p in args.exefs.glob("*") if p.name not in allowed)
    checks.append({"name": "unexpected ExeFS entries", "passed": not extras,
                   "errors": extras})
    check("main.npdm", args.exefs / "main.npdm", lock["npdm"], "npdm")
    check("control.nacp", args.nacp, lock["nacp"], "nacp")
    result = {"profile": lock["profile"], "passed": all(c["passed"] for c in checks),
              "scope": "ExeFS and NACP identity only; no RomFS or execution validation",
              "checks": checks}
    if args.output:
        destination = args.output.resolve()
        inputs = [args.exefs / m["name"] for m in lock["modules"]]
        inputs += [args.exefs / "main.npdm", args.nacp]
        if any(destination == source.resolve() for source in inputs) or destination.is_relative_to(args.exefs.resolve()):
            parser.error("report destination must be outside the input ExeFS and NACP")
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
