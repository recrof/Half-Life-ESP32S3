#!/bin/bash
# Screenshots of the host build and of a reference build at the same simulation frames,
# for visual comparison: tools/visual.sh <outdir> <map>...
#
# REFBIN: reference launcher (dynamic libraries next to it), REFBASE: its base directory
# (valve with the reference client and server). The frame time is fixed (host_framerate)
# so that both builds draw the same frames.
set -u
OUT=$1; shift
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"

shoot() { # <name> <bin> <basedir> <map>
	local name=$1 bin=$2 base=$3 map=$4
	rm -rf "$base/valve/scrshots"
	cat > "$base/valve/visual.cfg" <<EOF
scr_conspeed 100000
${VISUAL_EXTRA:-}
con_notifytime 0
wait 10
toggleconsole
wait 50
screenshot
sv_cheats 1
impulse 101
wait 40
screenshot
+left
wait 20
-left
wait 5
screenshot
wait 5
quit
EOF
	(cd "$base" && SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} SDL_VIDEO_HIGHDPI_DISABLED=1 XASH3D_BASEDIR=$PWD timeout 60 "$bin" -dev 0 -console -windowed \
		-width 320 -height 240 +host_framerate 0.05 +map "$map" +exec visual.cfg < /dev/null > "$OUT/$name-$map.log" 2>&1)
	local i=0
	for f in "$base"/valve/scrshots/*.png; do
		[ -f "$f" ] && cp "$f" "$OUT/$map-$i-$name.png"
		i=$((i + 1))
	done
	rm -f "$base/valve/visual.cfg"
}

for map in "$@"; do
	shoot new "$ROOT/host/hlhost" "$ROOT/gamedata" "$map"
	[ -n "${REFBIN:-}" ] && shoot ref "$REFBIN" "$REFBASE" "$map"
done
ls "$OUT"
