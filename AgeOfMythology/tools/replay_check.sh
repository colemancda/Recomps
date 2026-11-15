#!/bin/sh
# Replay a recorded game to its end, open the postgame statistics and compare them
# with a reference set (see docs/SURVEY.md, "Replaying the reference recording").
#
#   tools/replay_check.sh RECORDING.rcx OUTDIR [REFDIR]
#
# Copies RECORDING into the save folder of build/run as "aaa-replay.rcx" (first
# in the Recorded Games list), plays it headless at 1024x768 with Play Fast,
# then writes OUTDIR/{score,economy,military,improvements,mythology,timeline}.png.
# With REFDIR, each tab's table area is compared with REFDIR's; the script
# exits 1 if any differs. The reference set is the output of a run that was
# checked by eye against the same replay under Wine.
#
# Environment: AOM (default build/host/aom), ORIG (default orig), FRAMES (the
# frame the replay has ended by; default 28000), TIMEOUT (seconds, 1500).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 2 ] || { sed -n '2,13p' "$0"; exit 2; }
rec=$1 out=$2 ref=${3:-}
aom=${AOM:-$here/build/host/aom}
orig=${ORIG:-$here/orig}
end=${FRAMES:-28000}
save="$here/build/run/drive_c/Documents and Settings/Player/My Documents/My Games/Age of Mythology/Savegame"
mkdir -p "$save" "$out"
cp "$rec" "$save/aaa-replay.rcx"

# Main menu > Single Player > Recorded Games > first file > Open; Play Fast once
# loaded; at the end F10 > View Postgame, then each tab (y = 38).
steps="400:click 505 422;470:click 505 542;570:click 290 265;590:click 444 534;1500:click 297 691"
steps="$steps;$end:key 79 44;$((end + 60)):click 512 313"
dumps=""
f=$((end + 150))
for x in 126 280 434 589 741 896; do
    steps="$steps;$f:click $x 38"
    dumps="$dumps${dumps:+,}$((f + 40))"
    f=$((f + 50))
done
last=$((f - 10))
timeout "${TIMEOUT:-1500}" "$aom" --headless --exe "$orig/aomx.exe" --savedir "$here/build/run" \
    --input "$steps" --dump-frame "$dumps" --dump-path "$out/f%d.png" --frames "$last" \
    -- +noIntroCinematics >"$out/run.log" 2>&1 || { echo "replay run failed; see $out/run.log" >&2; exit 2; }

i=0
f=$((end + 190))
for tab in score economy military improvements mythology timeline; do
    [ -f "$out/f$f.png" ] || { echo "missing frame $f ($tab)" >&2; exit 2; }
    mv "$out/f$f.png" "$out/$tab.png"
    f=$((f + 50))
done
rm -f "$save/aaa-replay.rcx"
echo "postgame tabs in $out"

[ -n "$ref" ] || exit 0
python3 - "$out" "$ref" <<'EOF'
import sys
from PIL import Image, ImageChops
out, ref = sys.argv[1:]
bad = 0
for tab in ("score", "economy", "military", "improvements", "mythology", "timeline"):
    box = (40, 60, 985, 570) if tab != "timeline" else (216, 128, 944, 544)
    a = Image.open(f"{out}/{tab}.png").convert("RGB").crop(box)
    b = Image.open(f"{ref}/{tab}.png").convert("RGB").crop(box)
    diff = ImageChops.difference(a, b).getbbox()
    print(f"{tab:13s} {'same' if diff is None else 'DIFFERS in ' + str(diff)}")
    bad |= diff is not None
sys.exit(1 if bad else 0)
EOF
