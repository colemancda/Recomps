#!/usr/bin/env bash
# Package the aarch64 build as a port for ROCKNIX (Powkiddy X55, Retroid Pocket 5).
#
#   ./package.sh [--no-build]
#
# Writes build/dist/aom-rocknix/ and aom-rocknix.tar.gz:
#   AgeOfMythology.sh   launcher for the Ports menu -> /storage/roms/ports/
#   aom/aom             the program (aarch64, debug information removed)
#   aom/msxml4*.dll     system DLLs the program lifts, found next to it
#   aom/fonts/          Liberation fonts (stand-ins for Arial, Times, Courier)
#   aom/aom.conf, README.txt, BUILD-INFO.txt, game/ (empty: your files go here)
# No game data goes into the package; it is not committed or published.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cc="${AARCH64_CC:-/home/coleman/Developer/batocera.linux/output/h616/host/bin/aarch64-buildroot-linux-gnu-gcc}"
[ "${1:-}" = "--no-build" ] || "$here/build.sh" aarch64 >&2
out="$here/build/aarch64"
[ -x "$out/aom" ] || { echo "package.sh: $out/aom is missing; run ./build.sh aarch64" >&2; exit 2; }
dist="$here/build/dist"; pkg="$dist/aom-rocknix"
rm -rf "$pkg" "$dist/aom-rocknix.tar.gz"
mkdir -p "$pkg/aom/game" "$pkg/aom/save" "$pkg/aom/fonts"
"${cc%gcc}strip" -g -o "$pkg/aom/aom" "$out/aom"; chmod 755 "$pkg/aom/aom"
cp "$out"/msxml4.dll "$out"/msxml4r.dll "$pkg/aom/"
install -m 755 "$here/port/AgeOfMythology.sh" "$pkg/AgeOfMythology.sh"
install -m 644 "$here/port/README.txt" "$here/port/aom.conf" "$pkg/aom/"
cp /usr/share/fonts/truetype/liberation/Liberation{Sans,Serif,Mono}-*.ttf "$pkg/aom/fonts/"
echo "PUT-YOUR-GAME-FILES-HERE: the contents of your Age of Mythology folder" > "$pkg/aom/game/PUT-YOUR-GAME-FILES-HERE.txt"
{ echo "built $(date -u +%FT%TZ) from $(git -C "$here" rev-parse --short HEAD)"
  "${cc%gcc}readelf" -d "$pkg/aom/aom" | grep NEEDED
  "${cc%gcc}objdump" -T "$pkg/aom/aom" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1; } > "$pkg/aom/BUILD-INFO.txt"
tar -C "$dist" -czf "$dist/aom-rocknix.tar.gz" aom-rocknix
echo "wrote $pkg and $dist/aom-rocknix.tar.gz" >&2
