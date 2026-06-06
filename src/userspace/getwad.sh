#!/apps/sh.elf
# getwad.sh -- fetch a Doom WAD from a plain-HTTP mirror into /tmp.
#
# Makar's wget is HTTP-only (no TLS).  Default mirror is the LAN server at
# 10.0.0.111 (reachable from QEMU via slirp NAT).  WAD names there are
# upper-case (DOOM.WAD, DOOM2.WAD, NUTS.WAD, ...).
#
#   getwad.sh                 # fetches DOOM.WAD
#   getwad.sh DOOM2.WAD       # a specific wad
#   WADBASE=http://host/doom getwad.sh NUTS.WAD
#
# Then:  doom -iwad /tmp/DOOM.WAD

WAD=${1:-DOOM.WAD}
BASE=${WADBASE:-http://10.0.0.111/doom}

echo "getwad: $BASE/$WAD -> /tmp/$WAD"
if wget.elf "$BASE/$WAD" /tmp/$WAD; then
    echo "getwad: saved /tmp/$WAD"
    echo "run it: doom -iwad /tmp/$WAD"
else
    echo "getwad: failed (mirror reachable? http only, no https)"
fi
