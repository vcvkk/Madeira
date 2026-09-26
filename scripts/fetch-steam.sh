#!/bin/bash
# Assemble a ready-to-copy 64-bit Steam client folder from Valve's own CDN.
#
# Why not SteamSetup.exe: the installer is 32-bit x86 and Madeira runs x86-64
# only (ARM64EC + FEX, no 32-bit emulator). The client itself is 64-bit, so we
# fetch its update packages directly — the same zips the bootstrapper would
# download — verify each against the SHA-256 in Valve's manifest and unpack
# them. The manifest is also written as package/steam_client_win64.installed
# so the first launch sees an up-to-date client instead of self-updating.
#
# The result is what the "Steam Testing" button expects at
# C:\Program Files (x86)\Steam (see prepareSteamLaunch() in ContentView.swift).
#
# Usage: scripts/fetch-steam.sh [OUT_DIR]        (default: ./Steam)
#   PUSH=1 DEVICE_ID=<udid> scripts/fetch-steam.sh   also copies it to the phone
set -euo pipefail

OUT_DIR="${1:-$PWD/Steam}"
CDN="${STEAM_CDN:-https://media.steampowered.com/client}"
MANIFEST_NAME="steam_client_win64"
BUNDLE_ID="com.madeira.emulator"
DST_PATH='Documents/wine/drive_c/Program Files (x86)/Steam'

if command -v sha256sum >/dev/null; then SHA256="sha256sum"; else SHA256="shasum -a 256"; fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "==> Fetching manifest $CDN/$MANIFEST_NAME"
curl -fsS -o "$WORK_DIR/manifest" "$CDN/$MANIFEST_NAME"

# One "file sha2" line per top-level package. The steamrow/steamchina
# bootstrapper variants are skipped: the plain steam_win64 entry is the
# worldwide one (and is byte-identical to steamrow).
python3 - "$WORK_DIR/manifest" > "$WORK_DIR/list" <<'EOF'
import re, sys
text = open(sys.argv[1]).read()
seen = set()
for f, h in re.findall(r'"file"\s+"([^"]+)"\s+"size"\s+"\d+"\s+"sha2"\s+"([0-9a-f]{64})"', text):
    if 'steamrow' in f or 'steamchina' in f or f in seen:
        continue
    seen.add(f)
    print(f, h)
EOF

version="$(sed -n 's/^[[:space:]]*"version"[[:space:]]*"\([0-9]*\)".*/\1/p' "$WORK_DIR/manifest" | head -1)"
echo "==> Client version $version, $(wc -l < "$WORK_DIR/list" | tr -d ' ') packages"

mkdir -p "$OUT_DIR/package"
while read -r file sha; do
    echo "    $file"
    curl -fsS --retry 3 -o "$WORK_DIR/$file" "$CDN/$file"
    echo "$sha  $WORK_DIR/$file" | $SHA256 -c --quiet -
    unzip -qo "$WORK_DIR/$file" -d "$OUT_DIR"
    rm -f "$WORK_DIR/$file"
done < "$WORK_DIR/list"

# Some of Valve's zips store directory entries with Windows '\' separators.
# Info-ZIP unzip normalizes them; this catches any unzip that does not.
python3 - "$OUT_DIR" <<'EOF'
import os, shutil, sys
for dp, dns, fns in os.walk(sys.argv[1], topdown=False):
    for name in fns + dns:
        if '\\' not in name:
            continue
        src = os.path.join(dp, name)
        dst = os.path.join(dp, *name.split('\\'))
        if os.path.isdir(src):
            os.makedirs(dst, exist_ok=True)
            for x in os.listdir(src):
                shutil.move(os.path.join(src, x), os.path.join(dst, x))
            os.rmdir(src)
        else:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.move(src, dst)
EOF

cp "$WORK_DIR/manifest" "$OUT_DIR/package/$MANIFEST_NAME.manifest"
cp "$WORK_DIR/manifest" "$OUT_DIR/package/$MANIFEST_NAME.installed"

for f in steam.exe steamclient64.dll bin/cef/cef.win64/libcef.dll bin/cef/cef.win64/steamwebhelper.exe; do
    [[ -f "$OUT_DIR/$f" ]] || { echo "error: $f missing after unpack" >&2; exit 1; }
done
echo "==> Done: $OUT_DIR ($(du -sh "$OUT_DIR" | awk '{print $1}'))"

if [[ "${PUSH:-0}" == "1" ]]; then
    : "${DEVICE_ID:?set DEVICE_ID to the iPhone UDID (xcrun devicectl list devices)}"
    echo "==> Pushing to <app container>/$DST_PATH (launch Madeira once first so drive_c exists)"
    xcrun devicectl device copy to \
        --device "$DEVICE_ID" \
        --source "$OUT_DIR" \
        --destination "$DST_PATH" \
        --domain-type appDataContainer \
        --domain-identifier "$BUNDLE_ID"
fi
