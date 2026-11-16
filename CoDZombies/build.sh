#!/usr/bin/env bash
# Lift the game code and compile it.
#
#   ./build.sh [ios|host] [--relift]
#
# ios    arm64 iOS objects (xcrun --sdk iphoneos)   -> build/obj-ios/
# host   objects for this Mac                       -> build/obj-host/
#
# The survey (build/s3e_funcs.tsv) and the lifted C (build/gen/) are made when
# missing or with --relift. Linking waits for the runtime.
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
if [ $relift = 1 ] || [ ! -f build/gen/funcs.h ]; then
    python3 -I lift/lift.py --exe "$exe" --funcs build/s3e_funcs.tsv --out build/gen
fi

# -ffp-contract=off: VFPv2 rounds the product of vmla/vmls before the add.
flags=("$opt" -ffp-contract=off -fno-strict-aliasing -Wno-unused-label -Wno-parentheses-equality
       -Iruntime -Ibuild/gen)

compile() {
    local src="$1" obj="$out/$(basename "${1%.c}").o"
    if [ "$obj" -nt "$src" ] && [ "$obj" -nt runtime/cpu.h ]; then return 0; fi
    "${cc[@]}" "${flags[@]}" -c "$src" -o "$obj" 2> "${obj%.o}.log" || { echo "FAILED $src (see ${obj%.o}.log)" >&2; return 1; }
}

status=0
pids=()
for src in build/gen/chunk_*.c build/gen/tables.c; do
    compile "$src" &
    pids+=($!)
    if [ ${#pids[@]} -ge "$jobs" ]; then
        wait "${pids[0]}" || status=1
        pids=("${pids[@]:1}")
    fi
done
for p in "${pids[@]}"; do wait "$p" || status=1; done
[ $status = 0 ] && echo "compiled $(ls "$out"/*.o | wc -l | tr -d ' ') objects into $out ($(du -sh "$out" | cut -f1))"
exit $status
