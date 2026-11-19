#!/usr/bin/env bash
# The on-screen keyboard of the second screen, without a second screen or a server.
#
#   recomp/test/osk.sh [host|aarch64 ...]        (default: every build that exists)
#
# --second-screen on makes the keyboard window on a single screen, on SDL's dummy video driver.
# An input script taps keys (ktap, in thousandths of the window) and the program's --verbose log
# is judged: "hello" typed with the letter keys, Shift then w giving a capital, a held Shift
# that does not stick, Backspace as VK_BACK and Enter as VK_RETURN. kdump writes the keyboard
# window, which must exist and not be blank. The aarch64 build runs under qemu-aarch64 when its
# sysroot's SDL has the dummy video driver, and is skipped when it has not.
#
# Environment: RECOMP_EXE, GAMEDIR, TOKEN as for pad.sh. Output: recomp/build/osk.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
recomp="$(cd "$here/.." && pwd)"
root="$recomp"
exe="${RECOMP_EXE:-$root/orig/GunBound.gme}"
gamedir="${GAMEDIR:-$(dirname "$exe")}"
token="${TOKEN:-$(python3 "$recomp/tools/make_token.py" recomp)}"
out="$recomp/build/osk"
targets=("$@")
if [ ${#targets[@]} -eq 0 ]; then
    for t in host aarch64; do [ -x "$recomp/build/$t/gunbound" ] && targets+=("$t"); done
fi
mkdir -p "$out"
status=0

# keys of the lower layer: h 550,548  e 250,381  l 850,548  o 850,381  Shift 75,715  w 150,381
# Backspace 925,715  Enter 900,881
script="60:ktap 550 548;62:ktap 250 381;64:ktap 850 548;66:ktap 850 548;68:ktap 850 381"
script="$script;70:ktap 75 715;72:ktap 150 381;74:ktap 250 381;76:ktap 925 715;78:ktap 900 881;80:kdump"

for t in "${targets[@]}"; do
    prefix=""
    [ "$t" = aarch64 ] && prefix="qemu-aarch64 -L ${AARCH64_SYSROOT:-/home/coleman/Developer/batocera.linux/output/h616/host/aarch64-buildroot-linux-gnu/sysroot}"
    log="$out/$t.log"
    rm -f "$out/$t.png"
    SDL_VIDEODRIVER=dummy timeout 120 $prefix "$recomp/build/$t/gunbound" --exe "$exe" --gamedir "$gamedir" \
        --savedir "$out/save-$t" --cmdline "$token" --broker 127.0.0.1:8372 --no-sound --virtual-clock \
        --verbose --second-screen on --dump-osk "$out/$t.png" --input "$script" --frames 120 > "$log" 2>&1
    if grep -q "SDL_Init failed" "$log"; then
        echo "osk $t: skipped (this SDL has no dummy video driver: $(grep -o 'SDL_Init failed ([^)]*)' "$log" | head -1))"
        continue
    fi
    got="$(grep -o '^\[osk\] \(char [0-9]*\|key vk 0x[0-9a-f]*\)' "$log" | sed 's/^\[osk\] //' | tr '\n' ',')"
    want="char 104,char 101,char 108,char 108,char 111,char 87,char 101,key vk 0x8,key vk 0xd,"
    if [ "$got" = "$want" ] && [ -s "$out/$t.png" ]; then
        echo "osk $t: ok"
    else
        echo "osk $t: FAIL"
        echo "  got:  $got"
        echo "  want: $want"
        [ -s "$out/$t.png" ] || echo "  no keyboard window dump"
        status=1
    fi
done
exit $status
