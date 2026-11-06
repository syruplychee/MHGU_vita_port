#!/usr/bin/env python3
"""Check captured, shader-identified font quads against the native pixel grid."""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import struct

FONT_VERTEX_SHA = "97ca92a4e6f4a349c526adecd733c616d742c293354b301d4da0e5fb850eca95"
COPY_VERTEX_SHA = "ed669c11b36dbe538bf2e9d2a76e5b9e6f9b035d68a089fb56fe1705f12e8f8e"
COPY_FRAGMENT_SHA = "342f922d3034de651eadb2b69c8b75f4650d7f5159ddc59ada80f3eb9909cba6"


def extent(width, height, chosen_height):
    scale=min(chosen_height/720,540/height)
    return math.ceil(width*scale),math.ceil(height*scale)


def copy_ratio(draw, vertices, chosen_height):
    if (draw["stride"]!=16 or draw["count"]!=4 or draw["indexed"] or
            draw["attributes"][0]!=0x38200000 or len(vertices)!=64):
        raise ValueError("unreviewed present-copy vertex layout")
    q=[struct.unpack_from("<4f",vertices,k*16) for k in range(4)]
    if [(v[0],v[1]) for v in q]!=[(-1.,1.),(-1.,-1.),(1.,1.),(1.,-1.)]:
        raise ValueError("unreviewed present-copy geometry")
    target=extent(*draw["target"],chosen_height)
    source=extent(*draw["texture_size"],chosen_height)
    viewport=draw["viewport"]
    dst_w=viewport[2]*target[0]/draw["target"][0]
    dst_h=viewport[3]*target[1]/draw["target"][1]
    src_w=source[0]*abs(q[2][2]-q[0][2])
    src_h=source[1]*abs(q[1][3]-q[0][3])
    return dst_w/src_w,dst_h/src_h


def check(directory, shader_index, render_height=540):
    index = json.loads(shader_index.read_text())
    programs = {p["token"]:p for p in index["programs"]}
    counts = Counter()
    failures, captures, quads = [], 0, 0
    max_grid_error = 0.0
    copies=[]
    other_passes=0
    for path in sorted(directory.glob("font-probe-*.json")):
        draw = json.loads(path.read_text())
        program = programs[draw["vs"]]
        stage = index["stages"][program["vertex_stage"]]
        if not draw.get("font",1):
            if not draw.get("display_target"):
                other_passes+=1
                continue
            fragment=index["stages"][program["fragment_stage"]]
            if stage["stage_sha256"]!=COPY_VERTEX_SHA or fragment["stage_sha256"]!=COPY_FRAGMENT_SHA:
                raise ValueError(f"unreviewed UI-to-display copy shader: {path.name}")
            data=Path(str(path.with_suffix(""))+"-vertices.bin").read_bytes()
            ratios=copy_ratio(draw,data,render_height)
            copies.append(dict(capture=path.name,texture=draw["texture_va"],target=draw["target_va"],
                               source_pixel_to_target_pixel=ratios))
            if any(abs(v-1.0)>0.001 for v in ratios):
                failures.append(dict(capture=path.name,present_copy_ratio=ratios))
            continue
        if stage["stage_sha256"] != FONT_VERTEX_SHA:
            raise ValueError(f"unreviewed font vertex shader: {path.name}")
        if (draw["stride"] != 24 or draw["count"] % 4 or
                draw["attributes"][:3] != [0x38400000,0x11400600,0x38800800] or
                draw["target"] != [1280,720] or draw["viewport"] != [0,0,1280,720]):
            raise ValueError(f"unreviewed font draw layout: {path.name}")
        stem = path.with_suffix("")
        vertices = Path(str(stem)+"-vertices.bin").read_bytes()
        if len(vertices) != draw["count"]*24:
            raise ValueError(f"vertex capture length mismatch: {path.name}")
        world = struct.unpack_from("<16f",Path(str(stem)+"-vs-cbuf3.bin").read_bytes())
        projection = struct.unpack_from("<4f",Path(str(stem)+"-vs-cbuf4.bin").read_bytes())
        captures += 1
        native_width = render_height*16/9
        for offset in range(0,len(vertices),96):
            q = [struct.unpack_from("<3fI2f",vertices,offset+k*24) for k in range(4)]
            points = []
            for x,y,z,_,_,_ in q:
                clip_x = (x*world[0]+y*world[4]+z*world[8]+world[12])*projection[0]+projection[2]
                clip_y = (x*world[1]+y*world[5]+z*world[9]+world[13])*projection[1]+projection[3]
                points.append(((clip_x+1)*native_width/2,(1-clip_y)*render_height/2))
            uw = abs(q[1][4]-q[0][4])
            uh = abs(q[2][5]-q[0][5])
            if not uw or not uh:
                raise ValueError(f"empty or rotated UV quad requires review: {path.name}:{offset}")
            width = math.dist(points[0],points[1])
            height = math.dist(points[0],points[2])
            ratios = width/uw,height/uh
            grid_error = max(abs(v-round(v)) for point in points for v in point)
            max_grid_error = max(max_grid_error,grid_error)
            counts[tuple(round(v,5) for v in ratios)] += 1
            quads += 1
            ratio_error = max(abs(v-round(v)) for v in ratios)
            if grid_error > 0.001 or ratio_error > 0.001 or abs(ratios[0]-ratios[1]) > 0.001:
                failures.append(dict(capture=path.name,vertex=offset//24,
                                     grid_error=grid_error,texel_screen_ratio=ratios))
    if not quads:
        raise ValueError("no captured font quads")
    return dict(vertex_shader_sha256=FONT_VERTEX_SHA,render_height=render_height,
                captures=captures,quads=quads,max_grid_error=max_grid_error,
                texel_screen_ratios=[dict(x=k[0],y=k[1],quads=v) for k,v in sorted(counts.items())],
                present_copies=copies,other_intermediate_passes=other_passes,
                end_to_end_copy_verified=bool(copies),passed=not failures,failures=failures)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-dir",type=Path,required=True)
    parser.add_argument("--shader-index",type=Path,required=True)
    parser.add_argument("--render-height",type=int,choices=[360,540],default=540)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--require-present-copy",action="store_true")
    args=parser.parse_args()
    result=check(args.capture_dir,args.shader_index,args.render_height)
    args.output.write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({k:v for k,v in result.items() if k!="failures"}))
    if not result["passed"] or (args.require_present_copy and not result["end_to_end_copy_verified"]):
        raise SystemExit("font draw does not preserve the integer pixel grid")


if __name__=="__main__":
    main()
