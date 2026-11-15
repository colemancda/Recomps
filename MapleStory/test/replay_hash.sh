#!/usr/bin/env bash
# Whole-program check across architectures: replay a recorded game headless on the
# virtual clock, without sound and with the null Direct3D device, logging a hash of
# every frame's draw stream (RECOMP_D3D_HASH). The host build and the aarch64 build
# (under qemu-aarch64) must produce the same hash for every frame.
#
#   test/replay_hash.sh RECORDING.rcx [FRAMES]       (default 30000 frames)
#
# Environment:
#   ORIG     the game directory (default: orig)
#   SYSROOT  the aarch64 sysroot for qemu (default: the batocera h616 one)
#   ONLY     "host" or "aarch64" to run one build (its log is kept for a later run)
#
# Logs go to build/replay-hash/{host,aarch64}.log. The x86 run takes about 12
# minutes for the 14-minute reference game; the aarch64 one about 45 under qemu.
# Sound is off because Miles' service thread runs on real time and makes the
# main thread's clock reads, and so the frame timing, differ from run to run.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
rec="${1:?usage: $0 RECORDING.rcx [FRAMES]}"
frames="${2:-30000}"
orig="${ORIG:-$here/orig}"
sysroot="${SYSROOT:-/home/coleman/Developer/batocera.linux/output/h616/host/aarch64-buildroot-linux-gnu/sysroot}"
out="$here/build/replay-hash"
save="$here/build/run/drive_c/Documents and Settings/Player/My Documents/My Games/Age of Mythology/Savegame"
mkdir -p "$out" "$save"
cp "$rec" "$save/aaa-replay.rcx" # first in the Recorded Games list

# Single Player > Recorded Games > first file > Open, then Play Fast (1024x768 profile).
input="400:click 505 422;470:click 505 542;570:click 290 265;590:click 444 534;1500:click 297 691"
args=(--headless --virtual-clock --no-sound --exe "$orig/aomx.exe" --savedir "$here/build/run"
      --input "$input" --frames "$frames" -- +noIntroCinematics)

run() { # target
    local cmd=("$here/build/$1/aom")
    [ "$1" = aarch64 ] && cmd=(qemu-aarch64 -L "$sysroot" "${cmd[@]}")
    echo "running the $1 build ($frames frames)"
    RECOMP_NO_GL=1 RECOMP_D3D_HASH=1 "${cmd[@]}" "${args[@]}" >"$out/$1.raw" 2>&1 || {
        echo "$1 run failed; see $out/$1.raw" >&2
        exit 2
    }
    grep -a '^\[d3dhash\]' "$out/$1.raw" >"$out/$1.log"
}

case "${ONLY:-both}" in
host) run host ;;
aarch64) run aarch64 ;;
*) run host; run aarch64 ;;
esac
rm -f "$save/aaa-replay.rcx"

[ -f "$out/host.log" ] && [ -f "$out/aarch64.log" ] || exit 0
n=$(wc -l <"$out/host.log")
if cmp -s "$out/host.log" "$out/aarch64.log"; then
    echo "PASS  $n frames: identical draw streams on x86_64 and aarch64"
else
    first=$(diff "$out/host.log" "$out/aarch64.log" | grep -m1 '^<' || true)
    echo "FAIL  first difference: ${first:-length differs}"
    exit 1
fi
