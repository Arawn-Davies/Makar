#!/apps/sh.elf
# getwad.sh -- fetch a Doom WAD from a plain-HTTP mirror into /tmp.
#
# Makar's wget is HTTP-only (no TLS), and doomworld.com enforces https
# (plain http 403s), so the shareware WAD can't be pulled from there in-OS.
# Point WADBASE at a plain-HTTP mirror that serves the IWADs/PWADs.
#
#   getwad.sh                 # fetches doom1.wad
#   getwad.sh doom.wad        # fetches a specific wad
#   WADBASE=http://host/doom getwad.sh nuts.wad
#
# Then:  doom -iwad /tmp/<wad>

WAD=${1:-doom1.wad}
BASE=${WADBASE:-http://10.0.0.111/doom}

echo "getwad: $BASE/$WAD -> /tmp/$WAD"
if wget.elf "$BASE/$WAD" /tmp/$WAD; then
    echo "getwad: saved /tmp/$WAD"
    echo "run it: doom -iwad /tmp/$WAD"
else
    echo "getwad: failed (mirror reachable? http only, no https)"
fi
