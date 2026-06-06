#!/usr/bin/env bash
# getfreedoom.sh -- fetch the FreeDOOM IWADs into ./wads/ at build time.
#
# FreeDOOM (https://freedoom.github.io/) is a BSD-licensed, freely
# redistributable replacement for the copyrighted DOOM.WAD, so Makar can ship
# a playable DOOM out of the box without distributing id Software's assets.
# The WADs are NOT committed to git (wads/ is .gitignore'd); this script pulls
# them on demand and the userspace Makefile stages whatever *.wad it finds.
#
# Idempotent: a no-op if the WADs already exist.  Best-effort: a download
# failure (offline, mirror down) warns and exits 0 so it never breaks a build
# -- DOOM just falls back to any DOOM.WAD present, or prints its usage.
#
#   bash getfreedoom.sh            # fetch into ./wads/
#   FREEDOOM_VER=0.13.0 bash getfreedoom.sh
#
# Requires: curl (or wget) + unzip (or python3).  Auto-invoked from run.sh's
# ISO build path outside CI; run it manually any time.

set -u

VER="${FREEDOOM_VER:-0.13.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WADS="$HERE/wads"
URL="https://github.com/freedoom/freedoom/releases/download/v${VER}/freedoom-${VER}.zip"

mkdir -p "$WADS"

if [ -f "$WADS/freedoom1.wad" ] && [ -f "$WADS/freedoom2.wad" ]; then
    echo "getfreedoom: freedoom{1,2}.wad already present -- skipping."
    exit 0
fi

# Pick a downloader.
fetch() {  # fetch <url> <dest>
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 2 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$2" "$1"
    else
        echo "getfreedoom: need curl or wget" >&2; return 1
    fi
}

# Pick an extractor: unzip, else python3's zipfile module.
extract() {  # extract <zip> <member-glob> <destdir>
    if command -v unzip >/dev/null 2>&1; then
        unzip -joq "$1" "$2" -d "$3"
    elif command -v python3 >/dev/null 2>&1; then
        python3 - "$1" "$3" <<'PY'
import sys, zipfile, os, posixpath
zf, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zf) as z:
    for n in z.namelist():
        if n.lower().endswith(".wad"):
            data = z.read(n)
            out = os.path.join(dest, posixpath.basename(n).lower())
            with open(out, "wb") as f:
                f.write(data)
            print("getfreedoom: extracted", os.path.basename(out))
PY
    else
        echo "getfreedoom: need unzip or python3" >&2; return 1
    fi
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "getfreedoom: downloading FreeDOOM ${VER} ..."
if ! fetch "$URL" "$TMP/freedoom.zip"; then
    echo "getfreedoom: download failed (offline?) -- continuing without FreeDOOM." >&2
    exit 0
fi

if ! extract "$TMP/freedoom.zip" "*.wad" "$WADS"; then
    echo "getfreedoom: extraction failed -- continuing without FreeDOOM." >&2
    exit 0
fi

if [ -f "$WADS/freedoom1.wad" ]; then
    echo "getfreedoom: ready -> $WADS/freedoom1.wad (+freedoom2.wad)"
else
    echo "getfreedoom: WADs not found in archive -- continuing." >&2
fi
exit 0
