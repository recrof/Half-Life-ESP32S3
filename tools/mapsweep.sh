#!/bin/bash
# Load maps one after the other with the host build and report memory and errors:
#   tools/mapsweep.sh [map...]     (default: all the maps of gamedata/valve/maps)
#
# HLMEM=<size> sets the memory budget (static data included). Results in gamedata/sweep/
# (SWEEP_OUT: another directory, SWEEP_CMD: a console command run in each map). The 32-bit build (tools/arm32.sh run tools/mapsweep.sh) also
# reports the lowest free PSRAM of the device heap.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${SWEEP_OUT:-$ROOT/gamedata/sweep}
mkdir -p "$OUT"

if [ $# -eq 0 ]; then
	set -- $(ls "$ROOT/gamedata/valve/maps/" | grep '\.bsp$' | sed 's/\.bsp$//' | sort)
fi

printf "%-14s %-10s %8s %8s %9s %s\n" map result "in use" peak "min free" errors
for map in "$@"; do
	LOG=$OUT/$map.log "$ROOT/tools/hlrun.sh" "$map" 2 ${SWEEP_CMD:+"$SWEEP_CMD"} > /dev/null 2>&1
	log=$OUT/$map.log
	if grep -q "client connected" "$log"; then result=loaded; else result=FAILED; fi
	grep -q "out of memory" "$log" && result="$result/OOM"
	mem=$(grep "^HLMEM: .* in use" "$log" | head -1 | sed -E 's/HLMEM: ([0-9.]+) MB in use.*peak ([0-9.]+) MB.*/\1 \2/')
	minfree=$(grep "^HLMEM: .* in use" "$log" | head -1 | sed -nE 's/.*PSRAM free [0-9]+ \(min ([0-9]+),.*/\1/p')
	errs=$(grep -E "Sys_Error|Host_Error|Crash|error:|Error:" "$log" | grep -v "vgui_support\|SDL_GL_SetSwapInterval\|Could not load sound\|SDL:\|cp1251" | head -2 | sed -E 's/^\[[0-9:]+\] //' | tr '\n' ' ' | cut -c1-100)
	printf "%-14s %-10s %8s %8s %9s %s\n" "$map" "$result" ${mem:-- -} "${minfree:--}" "$errs"
done
