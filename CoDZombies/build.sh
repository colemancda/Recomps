#!/usr/bin/env bash
# Lift the game code and compile it.
#
#   ./build.sh [ios|host] [--relift]
#
# ios    arm64 iOS objects (xcrun --sdk iphoneos)   -> build/obj-ios/
# host   objects for this Mac                       -> build/obj-host/
#
# The survey (build/s3e_funcs.tsv), the import table (build/s3e_boundary.tsv)
# and the lifted C (build/gen/) are made when missing or with --relift. The host
# target also links the runtime into build/codz-host; run it as
#   build/codz-host orig/Payload/kiwi.app
#
# Environment:
#   JOBS   parallel compile jobs (default: number of CPUs)
#   OPT    optimisation level for the lifted code (default -O1)
set -euo pipefail

target="${1:-ios}"
relift=0
[ "${2:-}" = "--relift" ] && relift=1

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"
exe=orig/Payload/kiwi.app/kiwi
[ -f "$exe" ] || { echo "missing $exe; run tools/extract.sh first" >&2; exit 2; }
jobs="${JOBS:-$(sysctl -n hw.ncpu)}"
opt="${OPT:--O1}"

case "$target" in
ios)  cc=(xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=14.0) ;;
host) cc=(xcrun clang) ;;
*)    echo "usage: $0 [ios|host] [--relift]" >&2; exit 2 ;;
esac
out="build/obj-$target"

mkdir -p build "$out"
if [ $relift = 1 ] || [ ! -f build/s3e_funcs.tsv ]; then
    python3 -I tools/s3e_survey.py "$exe" --funcs build/s3e_funcs.tsv > build/survey.txt
fi
if [ $relift = 1 ] || [ ! -f build/s3e_boundary.tsv ]; then
    python3 -I tools/s3e_boundary.py "$exe" > build/s3e_boundary.tsv 2> build/boundary.txt
fi
if [ $relift = 1 ] || [ ! -f build/gen/imports.c ]; then
    python3 -I lift/lift.py --exe "$exe" --funcs build/s3e_funcs.tsv --imports build/s3e_boundary.tsv --out build/gen
fi

# -ffp-contract=off: VFPv2 rounds the product of vmla/vmls before the add.
# RT_DYNAMIC_BASE: guest memory goes wherever mmap puts it (the fixed default,
# 0x100000000, is where macOS and iOS load the executable).
flags=("$opt" -ffp-contract=off -fno-strict-aliasing -Wno-unused-label -Wno-parentheses-equality
       -DRT_DYNAMIC_BASE -Iruntime -Ibuild/gen)
# rebuild everything when the flags change
stamp="$out/flags"
if [ "$(cat "$stamp" 2>/dev/null)" != "${cc[*]} ${flags[*]}" ]; then
    rm -f "$out"/*.o
    echo "${cc[*]} ${flags[*]}" > "$stamp"
fi

compile() {
    local src="$1" obj="$out/$(basename "${1%.c}").o"
    if [ "$obj" -nt "$src" ] && [ "$obj" -nt runtime/cpu.h ]; then return 0; fi
    "${cc[@]}" "${flags[@]}" -c "$src" -o "$obj" 2> "${obj%.o}.log" || { echo "FAILED $src (see ${obj%.o}.log)" >&2; return 1; }
}

# objects of chunks that no longer exist (a re-lift can make fewer)
for obj in "$out"/chunk_*.o; do
    [ -e "$obj" ] || continue
    [ -f "build/gen/$(basename "${obj%.o}").c" ] || rm -f "$obj" "${obj%.o}.log"
done

status=0
pids=()
for src in build/gen/chunk_*.c build/gen/tables.c build/gen/imports.c; do
    compile "$src" &
    pids+=($!)
    if [ ${#pids[@]} -ge "$jobs" ]; then
        wait "${pids[0]}" || status=1
        pids=("${pids[@]:1}")
    fi
done
for p in "${pids[@]}"; do wait "$p" || status=1; done
[ $status = 0 ] || exit 1
echo "compiled $(ls "$out"/*.o | wc -l | tr -d ' ') lifted objects into $out ($(du -sh "$out" | cut -f1))"

# The runtime (always rebuilt; it's small) and, for the host, the program.
rt_flags=(-O1 -g -Wall -Wextra -Wno-unused-parameter -DRT_DYNAMIC_BASE -Iruntime -Ibuild/gen)
# liblzma (xz) for the derbh archives' LZMA streams: Homebrew's on the host
lzma_prefix="${LZMA_PREFIX:-/opt/homebrew}"
[ "$target" = host ] && rt_flags+=(-I"$lzma_prefix/include")
mkdir -p "$out/runtime"
for src in runtime/*.c; do
    "${cc[@]}" "${rt_flags[@]}" -c "$src" -o "$out/runtime/$(basename "${src%.c}").o"
done
if [ "$target" = host ]; then
    "${cc[@]}" -o build/codz-host "$out"/*.o "$out"/runtime/*.o -lz "$lzma_prefix/lib/liblzma.a" \
        -framework OpenGL -framework CoreGraphics -framework ImageIO -framework CoreFoundation
    echo "linked build/codz-host"
fi
