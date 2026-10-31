#!/usr/bin/env bash
# Report, per translated program, the exact reason shader_ir rejected the GLSL.
# Run inside WSL from the repo root: bash tools/shader/cg_reject_reasons.sh
set -u
out_dir=${1:-contents/build/vp04-translate}
for d in "$out_dir"/s*/; do
    [ -d "$d" ] || continue
    n=$(basename "$d")
    g="$d/shader.normalized.glsl"
    [ -f "$g" ] || g="$d/shader.glsl"
    [ -f "$g" ] || continue
    case "$n" in
        s1_*) st=vertex ;;
        *)    st=fragment ;;
    esac
    msg=$(python3 tools/shader/shader_ir.py --glsl "$g" --stage "$st" --output /tmp/vp-reject.cg 2>&1 | tail -1)
    printf '%-14s %-9s %s\n' "$n" "$st" "$msg"
done
