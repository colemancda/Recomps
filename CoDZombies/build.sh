#!/usr/bin/env bash
# Lift the game code and compile it.
#
#   ./build.sh [ios|sim|host] [--relift]
#
# ios    arm64 iOS device              -> build/obj-ios/, build/CoDZombies-ios.app
# sim    arm64 iOS Simulator           -> build/obj-sim/, build/CoDZombies-sim.app
# host   this Mac, headless or offscreen GL -> build/obj-host/, build/codz-host
#
# The survey (build/s3e_funcs.tsv), the import table (build/s3e_boundary.tsv)
# and the lifted C (build/gen/) are made when missing or with --relift. Run the
# host program as
#   build/codz-host orig/Payload/kiwi.app
# The apps carry the original executable and the game's archives from
# orig/Payload/kiwi.app. The simulator app is signed ad hoc; the device app is
# signed with $CODESIGN_IDENTITY when it is set, and left unsigned otherwise.
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
sim)  cc=(xcrun --sdk iphonesimulator clang -target arm64-apple-ios14.0-simulator) ;;
host) cc=(xcrun clang) ;;
*)    echo "usage: $0 [ios|sim|host] [--relift]" >&2; exit 2 ;;
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

# The LZMA SDK's decoder (Igor Pavlov, public domain) for the derbh archives'
# LZMA streams. It isn't kept in the repository: it is fetched from 7-Zip's
# repository at a pinned release into build/third_party and checked by SHA-256.
lzma_tag=26.06
lzma_dir=build/third_party/lzma-$lzma_tag
lzma_files=(
    "LzmaDec.c 4e6ec665a6df01f1722e2b2fc36dc5e3aa7c3890e0f8cb11c6a07c9a01f3309b"
    "LzmaDec.h 3aaf07b4ae4173a2d103179455dc7089b5ddbc7fc3db3c0e40964a7499c69266"
    "7zTypes.h a5d03b8cb65cd3a2a56bde46dd68fb356d2ebacd9d70283e7d4d20b4e0a64713"
    "Precomp.h c8903f2e36a771a272d8981e1e9ffed5ba16ca3d2f3dca6d1414d2340a6829d0"
    "Compiler.h d5e42be77d26beaa8c81d3e62ef99fc5685736cfc079779fdb9154d05fa8bc4a"
)
mkdir -p "$lzma_dir"
for entry in "${lzma_files[@]}"; do
    name="${entry% *}" sum="${entry#* }"
    if [ ! -f "$lzma_dir/$name" ]; then
        echo "fetching LZMA SDK $lzma_tag: $name"
        curl -sfL -o "$lzma_dir/$name.part" "https://raw.githubusercontent.com/ip7z/7zip/$lzma_tag/C/$name" \
            || { echo "download of $name failed" >&2; exit 1; }
        mv "$lzma_dir/$name.part" "$lzma_dir/$name"
    fi
    if [ "$(shasum -a 256 "$lzma_dir/$name" | cut -d' ' -f1)" != "$sum" ]; then
        echo "$lzma_dir/$name doesn't match its SHA-256; delete it to fetch it again" >&2
        exit 1
    fi
done
rt_flags+=(-I"$lzma_dir")

mkdir -p "$out/runtime"
rm -f "$out"/runtime/*.o
for src in runtime/*.c "$lzma_dir/LzmaDec.c"; do
    "${cc[@]}" "${rt_flags[@]}" -c "$src" -o "$out/runtime/$(basename "${src%.c}").o"
done
if [ "$target" = host ]; then
    "${cc[@]}" -o build/codz-host "$out"/*.o "$out"/runtime/*.o -lz \
        -framework OpenGL -framework CoreGraphics -framework ImageIO -framework CoreFoundation
    echo "linked build/codz-host"
else
    # The app: ios/app.m around the runtime, the original executable (the
    # loader reads its segments) and the archives as resources.
    app="build/CoDZombies-$target.app"
    rm -rf "$app"
    mkdir -p "$app"
    "${cc[@]}" "${rt_flags[@]}" -fobjc-arc -c ios/app.m -o "$out/runtime/app.o"
    "${cc[@]}" -o "$app/CoDZombies" "$out"/*.o "$out"/runtime/*.o -lz \
        -framework UIKit -framework OpenGLES -framework QuartzCore -framework CoreGraphics -framework Foundation
    cp ios/Info.plist "$app/Info.plist"
    platform=iPhoneOS
    [ "$target" = sim ] && platform=iPhoneSimulator
    plutil -insert CFBundleSupportedPlatforms -json "[\"$platform\"]" "$app/Info.plist"
    cp orig/Payload/kiwi.app/kiwi orig/Payload/kiwi.app/*.dz "$app/"
    cp -R orig/Payload/kiwi.app/videos "$app/"
    if [ "$target" = sim ]; then
        codesign -s - --force "$app" > /dev/null
    elif [ -n "${CODESIGN_IDENTITY:-}" ]; then
        codesign -s "$CODESIGN_IDENTITY" --force "$app" > /dev/null
    fi
    echo "linked $app ($(du -sh "$app" | cut -f1))"
fi
