#!/bin/bash
# Records a demo of the tram ride with the host build (tools/arm32.sh run tools/recdemo.sh <name> <seconds>):
# gamedata/valve/<name>.dem from the start of c0a0
cd "$(dirname "$0")/../gamedata"
NAME=${1:-ride}; SECS=${2:-330}
BIN=${HLBIN:-../host/hlhost}; LOG=rec-$NAME.log
C=$(mktemp -t rec.XXXXXX); : > "$C"
tail -f "$C" | SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy XASH3D_BASEDIR=$PWD ${HLEXEC:-} "$BIN" -dev 2 -windowed -width 320 -height 240 +map c0a0 > "$LOG" 2>&1 &
for i in $(seq 1 600); do grep -q "client connected" "$LOG" && break; sleep 0.1; done
sleep 3
echo "record $NAME" >> "$C"; sleep "$SECS"
echo stop >> "$C"; sleep 1; echo quit >> "$C"; sleep 2
pkill -f "$BIN"; sleep 1; pkill -9 -f "$BIN"; pkill -f "tail -f $C"; rm -f "$C"
grep -a "TDTITLE\|Sys_Error\|Crash" "$LOG" | head -3; ls -la valve/$NAME.dem
