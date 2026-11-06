#!/usr/bin/env python3
"""Verify the locked A32 font hooks, guards and direct-branch patch windows."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools/input"))
from nso_reloc_model import decode
import lz4.block


def branch_target(offset, word):
    if word >> 28 == 15 or word & 0x0e000000 != 0x0a000000:
        return None
    immediate = word & 0xffffff
    if immediate & 0x800000:
        immediate -= 0x1000000
    return offset+8+immediate*4


def audit(exefs):
    lock=json.loads((ROOT/"config/target-1.4.0.json").read_text())
    main=next(m for m in lock["modules"] if m["name"]=="main")
    image=decode(exefs/"main",main,lz4.block.decompress)
    header=(ROOT/"src/loader/resource_experiment.h").read_text()
    pattern=r'\{(0x[0-9a-f]+)u, (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, "(font-(?:quad|uv)-[^"]+)",\s*"([0-9a-f]{64})"'
    sites=[]
    for match in re.finditer(pattern,header):
        offset,first,second,service=(int(v,16) for v in match.groups()[:4])
        name,expected=match.groups()[4:]
        data=bytes(image.image[offset:offset+64])
        if struct.unpack_from("<2I",data)!=(first,second) or hashlib.sha256(data).hexdigest()!=expected:
            raise ValueError(f"font patch identity mismatch: {name}")
        sites.append(dict(name=name,offset=offset,service=service,
                          original_bytes=data[:8].hex(),guard_bytes=64,guard_sha256=expected,
                          direct_callers=[],interior_branch_candidates=[]))
    if len(sites)!=4:
        raise ValueError("expected exactly four native font patch sites")
    for offset in range(0,image.segments[0].size-3,4):
        word=struct.unpack_from("<I",image.image,offset)[0]
        target=branch_target(offset,word)
        for site in sites:
            if target==site["offset"]:
                site["direct_callers"].append(hex(offset))
            if target==site["offset"]+4:
                site["interior_branch_candidates"].append(hex(offset))
    if any(site["interior_branch_candidates"] for site in sites):
        raise ValueError("direct branch enters a replaced literal word; patch window needs review")
    for site in sites:
        site["offset"]=hex(site["offset"])
        site["service"]=hex(site["service"])
    display_offset=int(re.search(r'VP_NATIVE_DISPLAY_GUARD_OFFSET (0x[0-9a-f]+)u',header)[1],16)
    display_bytes=int(re.search(r'VP_NATIVE_DISPLAY_GUARD_BYTES (0x[0-9a-f]+)u',header)[1],16)
    display_sha=re.search(r'VP_NATIVE_DISPLAY_GUARD_SHA "([0-9a-f]+)"',header)[1]
    if hashlib.sha256(image.image[display_offset:display_offset+display_bytes]).hexdigest()!=display_sha:
        raise ValueError("native display preset guard mismatch")
    display=[]
    block=header.split("vp_native_display_words[] = {",1)[1].split("};",1)[0]
    for row in re.finditer(r'\{(0x[0-9a-f]+)u,(0x[0-9a-f]+)u,(0x[0-9a-f]+)u\}',block):
        offset,original,replacement=(int(v,16) for v in row.groups())
        if struct.unpack_from("<I",image.image,offset)[0]!=original:
            raise ValueError("native display original word mismatch")
        display.append(dict(offset=hex(offset),original=f"{original:08x}",replacement=f"{replacement:08x}"))
    if len(display)!=2:
        raise ValueError("expected two native display preset words")
    return dict(input_sha256=main["sha256"],decoded_sha256=hashlib.sha256(image.image).hexdigest(),
                sites=sites,display_preset=dict(offset=hex(display_offset),guard_bytes=display_bytes,
                    guard_sha256=display_sha,words=display),passed=True,
                limit="Static direct A32 branches only; native bridge reach and ABI are also checked during loading.")


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exefs",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    result=audit(args.exefs)
    args.output.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result))
