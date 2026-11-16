#!/usr/bin/env bash
# Unpack the decrypted Call of Duty: World at War Zombies 1.5.0 IPA into orig/.
#
#   tools/extract.sh [path/to/decrypted.ipa]
#
# Default IPA: $COD_IPA, else the decrypted copy in /Volumes/4TB/Downloads/IPA/Decrypted.
# The App Store IPA (COD Zombies 1.5.0.ipa) is FairPlay-encrypted (cryptid 1) and
# is refused; only a decrypted copy can be lifted or run.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
ipa="${1:-${COD_IPA:-/Volumes/4TB/Downloads/IPA/Decrypted/com.activision.callofduty-1.5.0-decrypted.ipa}}"
orig="$here/orig"

[ -f "$ipa" ] || { echo "IPA not found: $ipa" >&2; exit 2; }

rm -rf "$orig"
mkdir -p "$orig"
unzip -q "$ipa" -d "$orig"
app="$orig/Payload/kiwi.app"
exe="$app/kiwi"
[ -f "$exe" ] || { echo "no kiwi.app/kiwi in $ipa" >&2; exit 1; }

version="$(plutil -extract CFBundleVersion raw "$app/Info.plist")"
cryptid="$(otool -l "$exe" | awk '/cryptid/{print $2; exit}')"
echo "version $version, $(lipo -info "$exe" | sed 's/.*: //'), cryptid ${cryptid:-none}"
if [ "${cryptid:-0}" != 0 ]; then
    echo "binary is still encrypted; use a decrypted IPA" >&2
    exit 1
fi
echo "sha1 $(shasum "$exe" | cut -c1-40)"
