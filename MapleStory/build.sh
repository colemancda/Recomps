#!/usr/bin/env bash
# Build the statically recompiled MapleStory client (GMSv62.exe and, as the boot
# progresses, the Wizet engine DLLs lifted with it).
#
#   ./build.sh [host|aarch64] [--relift]
#
# host     native build with the system compiler      -> build/host/maple
# aarch64  cross build for Linux/arm64 (glibc)         -> build/aarch64/maple
#
# Each image is lifted into build/gen/<module>/ with lift.py --module, so its
# functions are f_<module>_<addr>; build/gen/images.c lists them for the runtime.
#
# Environment: ORIG, AARCH64_CC, JOBS, OPT, EXTRA_CFLAGS, LIFT_FLAGS, NO_SDL,
# VARIANT  -- as in the Age of Mythology build.
set -euo pipefail

target="${1:-host}"
relift=0
[ "${2:-}" = "--relift" ] && relift=1

here="$(cd "$(dirname "$0")" && pwd)"
orig="${ORIG:-$here/orig}"
build="$here/build"
variant="${VARIANT:-}"
gen="$build/gen$variant"
out="$build/$target$variant"
jobs="${JOBS:-$(nproc)}"
opt="${OPT:--O1}"

# module  file           lift base (empty: the PE's own)   function list (optional)   hooks
# The EXE comes first: the runtime takes rt_images[0] as the program. The engine
# DLLs (PCOM, NameSpace, ResMan, Shape2D, Canvas, Gr2D_DX8, Sound_DX8) are loaded
# by the client at start-up; ijl15 decodes the .wz JPEGs. WzMss and GameGuard
# (npkcrypt) are stubbed in the runtime. Each DLL keeps its own ImageBase.
modules=(
    "msmain    GMSv62.exe      -   $build/survey/GMSv62.functions.csv     -"
    "msvcrt    $orig/syslib/msvcrt.dll   -   $build/survey/msvcrt.functions.csv   -"
    "pcom      PCOM.dll        -   $build/survey/PCOM.functions.csv        -"
    "namespace NameSpace.dll   -   $build/survey/NameSpace.functions.csv   -"
    "resman    ResMan.dll      -   $build/survey/ResMan.functions.csv      -"
    "shape2d   Shape2D.dll     -   $build/survey/Shape2D.functions.csv     -"
    "canvas    Canvas.dll      -   $build/survey/Canvas.functions.csv      -"
    "gr2d      Gr2D_DX8.dll    -   $build/survey/Gr2D_DX8.functions.csv    -"
    "sounddx8  Sound_DX8.dll   -   $build/survey/Sound_DX8.functions.csv   -"
    "ijl15     ijl15.dll       -   $build/survey/ijl15.functions.csv       -"
    "zlz       ZLZ.dll         -   $build/survey/ZLZ.functions.csv         -"
)

# Each function list is Ghidra's export (<stem>.ghidra.csv) plus the code-pointer
# targets in .data/.rdata that Ghidra misses (the CRT initialiser table and the
# C++ vtables), appended by tools/data_code_pointers.py. Without these the EXE
# faults in _initterm at the first static constructor, and the engine DLLs miss
# their indirectly-called vtable methods.
for line in "${modules[@]}"; do
    read -r _ file _ merged_csv _ <<<"$line"
    [ -n "$merged_csv" ] && [ "$merged_csv" != - ] || continue
    ghidra_csv="${merged_csv%.functions.csv}.ghidra.csv"
    [ -f "$ghidra_csv" ] || continue
    if [ ! -f "$merged_csv" ] || [ "$ghidra_csv" -nt "$merged_csv" ] || \
       [ "$here/tools/data_code_pointers.py" -nt "$merged_csv" ]; then
        echo "== merging data-section code pointers for $file"
        cp "$ghidra_csv" "$merged_csv"
        case "$file" in /*) mpe="$file" ;; *) mpe="$orig/$file" ;; esac
        python3 "$here/tools/data_code_pointers.py" "$mpe" "$ghidra_csv" >>"$merged_csv"
    fi
done

case "$target" in
host) cc="${CC:-gcc}" ;;
aarch64) cc="${AARCH64_CC:-/home/coleman/Developer/batocera.linux/output/h616/host/bin/aarch64-buildroot-linux-gnu-gcc}" ;;
*)
    echo "usage: $0 [host|aarch64] [--relift]" >&2
    exit 2
    ;;
esac
command -v "$cc" >/dev/null || { echo "compiler not found: $cc" >&2; exit 2; }

if [ "${NO_SDL:-0}" = 1 ]; then
    sdl_cflags="-DRT_NO_SDL"
    sdl_libs=""
elif [ "$target" = host ]; then
    sdl_cflags="$(sdl2-config --cflags)"
    sdl_libs="$(sdl2-config --libs)"
else
    sdl_cflags="-I$("$cc" -print-sysroot)/usr/include/SDL2 -D_REENTRANT"
    sdl_libs="-lSDL2"
fi

# FreeType renders the fonts GDI hands out (gdi.c); EGL and OpenGL ES 3 draw Direct3D (d3d8gl.c).
if [ "$target" = host ]; then
    ft_cflags="$(pkg-config --cflags freetype2)"
    ft_libs="$(pkg-config --libs freetype2) $(pkg-config --libs egl glesv2)"
else
    ft_cflags="-I$("$cc" -print-sysroot)/usr/include/freetype2"
    ft_libs="-lfreetype -lEGL -lGLESv2"
fi

# ---- 1. lift each image (redone when the lifter or its inputs change)
mkdir -p "$gen" "$out/obj"
pids=()
for line in "${modules[@]}"; do
    read -r mod file rebase csv hooks <<<"$line"
    case "$file" in /*) pe="$file" ;; *) pe="$orig/$file" ;; esac
    [ -f "$pe" ] || { echo "not found: $pe (set ORIG)" >&2; exit 2; }
    [ "$csv" = - ] || [ -f "$csv" ] || csv=-
    dir="$gen/$mod"
    stamp="$dir/.stamp"
    need=$relift
    [ -f "$stamp" ] || need=1
    if [ "$need" = 0 ]; then
        for f in "$here"/lift/*.py "$here/build.sh" "$pe" ${csv#-}; do
            [ "$f" -nt "$stamp" ] && need=1
        done
    fi
    [ "$need" = 1 ] || continue
    mkdir -p "$dir"
    args=(--pe "$pe" --module "$mod" --out "$dir")
    [ "$rebase" = - ] || args+=(--rebase "$rebase")
    [ "$csv" = - ] || args+=(--csv "$csv")
    [ "$hooks" = - ] || args+=(--hook "$hooks")
    echo "== lifting $file"
    # shellcheck disable=SC2086
    (python3 "$here/lift/lift.py" "${args[@]}" ${LIFT_FLAGS:-} >"$dir/lift.log" 2>&1 && touch "$stamp") &
    pids+=($!)
done
for p in "${pids[@]}"; do
    wait "$p" || { echo "lift failed; see $gen/*/lift.log" >&2; exit 1; }
done

{
    echo "/* Generated by build.sh: the lifted images, EXE first. */"
    echo '#include "rt.h"'
    for line in "${modules[@]}"; do read -r mod _ <<<"$line"; echo "extern const RtImageDesc rt_image_$mod;"; done
    echo "const RtImageDesc *const rt_images[] = {"
    for line in "${modules[@]}"; do read -r mod _ <<<"$line"; echo "    &rt_image_$mod,"; done
    echo "};"
    echo "const unsigned rt_nimages = ${#modules[@]};"
} >"$gen/images.c.new"
cmp -s "$gen/images.c.new" "$gen/images.c" 2>/dev/null && rm "$gen/images.c.new" || mv "$gen/images.c.new" "$gen/images.c"

# ---- 2. compile + link through ninja
cflags_common="-std=gnu11 -pthread -fno-strict-aliasing -fwrapv -fno-stack-protector -I$here/runtime ${EXTRA_CFLAGS:-}"
{
    echo "cc = $cc"
    echo "gen_cflags = $opt -foptimize-sibling-calls $cflags_common -w"
    echo "rt_cflags = -O2 -g $cflags_common $sdl_cflags $ft_cflags -Wall -Wextra -Wno-unused-parameter"
    echo "rule gen"
    echo "  command = \$cc \$gen_cflags \$inc -c \$in -o \$out"
    echo "  description = CC \$out"
    echo "rule rt"
    echo "  command = \$cc \$rt_cflags -MMD -MF \$out.d -c \$in -o \$out"
    echo "  depfile = \$out.d"
    echo "  description = CC \$out"
    echo "rule link"
    echo "  command = \$cc -o \$out \$in -lm -pthread $sdl_libs $ft_libs"
    echo "  description = LINK \$out"
    objs=""
    for line in "${modules[@]}"; do
        read -r mod _ <<<"$line"
        for src in "$gen/$mod"/chunk_*.c "$gen/$mod/tables.c"; do
            o="obj/$mod/$(basename "${src%.c}").o"
            deps="$here/runtime/cpu.h"
            [ "$(basename "$src")" = tables.c ] && deps="$deps $here/runtime/rt.h $gen/$mod/funcs.h"
            echo "build $o: gen $src | $deps"
            echo "  inc = -I$gen/$mod"
            objs="$objs $o"
        done
    done
    echo "build obj/images.o: gen $gen/images.c | $here/runtime/rt.h"
    objs="$objs obj/images.o"
    for src in "$here"/runtime/*.c; do
        o="obj/rt_$(basename "${src%.c}").o"
        echo "build $o: rt $src"
        objs="$objs $o"
    done
    echo "build maple: link $objs"
    echo "default maple"
} >"$out/build.ninja"

echo "== compiling for $target with $cc ($opt, -j$jobs)"
t0=$(date +%s)
ninja -C "$out" -j "$jobs"
echo "== build took $(( $(date +%s) - t0 ))s -> $out/maple"
