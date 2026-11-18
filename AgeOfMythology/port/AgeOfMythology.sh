#!/bin/bash
# Age of Mythology: The Titans (recompiled) as a ROCKNIX port.
#
# This file goes into /storage/roms/ports/ next to the directory aom/, which
# holds the program and, in aom/game/, your own copy of the game.
# Anything given on the command line is passed on to the program
# (try: AgeOfMythology.sh --frames 200 --dump-frame 150).

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
    controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
    controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
    controlfolder="$XDG_DATA_HOME/PortMaster"
else
    controlfolder="/roms/ports/PortMaster"
fi
if [ -f "$controlfolder/control.txt" ]; then
    # shellcheck disable=SC1090,SC1091
    source "$controlfolder/control.txt"
    # shellcheck disable=SC1090
    [ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
    get_controls
    unset LD_LIBRARY_PATH
elif [ -f /etc/profile ]; then
    # shellcheck disable=SC1091
    . /etc/profile
fi

here="$(cd "$(dirname "$0")" && pwd)"
port="$here/aom"
[ -d "$port" ] || port="$here"
log="$port/log.txt"
cd "$port" || exit 1
exec > >(tee "$log") 2>&1
echo "== $(date) Age of Mythology port starting in $port"

AOM_EXE=aomx.exe   # aomx.exe is The Titans, aom.exe the base game
AOM_OPTIONS=""
# shellcheck disable=SC1091
[ -f "$port/aom.conf" ] && . "$port/aom.conf"

fail() {
    echo "Age of Mythology: $*"
    echo "(this message is in $log)"
    sleep 5
    exit 1
}

find_file() { find "$1" -maxdepth 1 -iname "$2" 2>/dev/null | head -n 1; }
game="$port/game"
exe="$(find_file "$game" "$AOM_EXE")"
[ -n "$exe" ] || fail "no $AOM_EXE in $game: copy your game directory's contents there (see README.txt)"
mkdir -p "$port/save"

# the program must be executable, which a FAT or exFAT card does not allow
bin="$port/aom"
[ -f "$bin" ] || fail "the program $bin is missing"
if ! chmod +x "$bin" 2>/dev/null || [ ! -x "$bin" ]; then
    rm -rf /tmp/aom-port && mkdir -p /tmp/aom-port
    cp "$bin" "$port"/msxml4*.dll /tmp/aom-port/ && chmod +x /tmp/aom-port/aom || fail "cannot make the program executable"
    bin=/tmp/aom-port/aom
    echo "== running a copy of the program from $bin (the port directory does not allow execution)"
fi
run=("$bin")
if [ ! -e /lib/ld-linux-aarch64.so.1 ]; then
    for ld in /usr/lib/ld-linux-aarch64.so.1 /lib64/ld-linux-aarch64.so.1; do
        [ -e "$ld" ] && { run=("$ld" "$bin"); break; }
    done
fi

if [ -z "${SDL_GAMECONTROLLERCONFIG_FILE:-}" ]; then
    for db in /storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt /usr/config/SDL-GameControllerDB/gamecontrollerdb.txt; do
        [ -f "$db" ] && { export SDL_GAMECONTROLLERCONFIG_FILE="$db"; break; }
    done
fi

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run/0-runtime-dir}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
unset DISPLAY
type pm_platform_helper > /dev/null 2>&1 && pm_platform_helper "$bin"

pad=()
[ -f "$port/pad.cfg" ] && pad=(--pad-config "$port/pad.cfg")
fonts=()
[ -d "$port/fonts" ] && fonts=(--font-dir "$port/fonts")

echo "== ${run[*]} $exe"
# shellcheck disable=SC2086
"${run[@]}" --exe "$exe" --gamedir "$game" --savedir "$port/save" \
    --fullscreen --soft-cursor --scale-mode fit "${fonts[@]}" "${pad[@]}" $AOM_OPTIONS "$@"
status=$?
echo "== exit status $status"
type pm_finish > /dev/null 2>&1 && pm_finish
exit $status
