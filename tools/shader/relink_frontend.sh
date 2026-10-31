#!/usr/bin/env bash
# Recompile only the Maxwell->GLSL adapter and relink it against the object files
# that tools/shader/build_frontend.py already produced.
#
# Why this exists: a full build_frontend.py run rebuilds 140 translation units on
# a Linux/WSL host. When the only change is the adapter (tools/shader/
# maxwell_glsl.cpp), recompiling that one unit and relinking is enough.
#
# The adapter's own object file is excluded from the link set by looking for the
# object that defines the adapter's `main`, so the fresh object never collides
# with the stale one.
#
# Usage: bash tools/shader/relink_frontend.sh [--output PATH]
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_dir"

vendor="local/shader-tools/shader-compiler-rs/vendor"
build="local/shader-tools/frontend-build"
adapter="tools/shader/maxwell_glsl.cpp"
output="$build/maxwell-glsl"

while [ $# -gt 0 ]; do
    case "$1" in
        --output) output="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -d "$vendor" ] || { echo "missing pinned upstream: $vendor" >&2; exit 1; }
[ -f "$adapter" ] || { echo "missing adapter: $adapter" >&2; exit 1; }
count=$(find "$build" -maxdepth 1 -name '*.o' | wc -l)
[ "$count" -gt 0 ] || { echo "no object files in $build; run build_frontend.py first" >&2; exit 1; }

includes=(
    "-I$vendor"
    "-I$vendor/range-v3/include"
    "-I$vendor/fmt/include"
    "-I$vendor/boost/config/include"
    "-I$vendor/boost/assert/include"
    "-I$vendor/boost/move/include"
    "-I$vendor/boost/intrusive/include"
    "-I$vendor/boost/container/include"
)

tmp_obj=$(mktemp /tmp/vp-adapter-XXXXXX.o)
trap 'rm -f "$tmp_obj" "$tmp_obj.link"' EXIT

echo "compiling $adapter"
g++ -std=c++20 -O1 -DFMT_HEADER_ONLY -w "${includes[@]}" -c "$adapter" -o "$tmp_obj"

# Identify which prebuilt object already provides the adapter's main() so it can
# be dropped from the link set; keeping both would be a duplicate-symbol error.
stale=""
for obj in "$build"/*.o; do
    if nm -g --defined-only "$obj" 2>/dev/null | grep -qE ' T main$'; then
        stale="$obj"
        break
    fi
done
if [ -n "$stale" ]; then
    echo "excluding previous adapter object: $(basename "$stale")"
fi

echo "linking -> $output"
objs=()
for obj in "$build"/*.o; do
    [ "$obj" = "$stale" ] && continue
    objs+=("$obj")
done
g++ "$tmp_obj" "${objs[@]}" -o "$output"

echo "built: $output ($(stat -c%s "$output") bytes)"
sha256sum "$output"
