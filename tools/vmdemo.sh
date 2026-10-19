#!/bin/bash
# Plays a demo with timedemo in the host build (tools/arm32.sh run tools/vmdemo.sh <demo>), log in gamedata/vmdemo-<demo>.log
cd "$(dirname "$0")/../gamedata"
DEMO=${1:-bench}; LOG=${LOG:-vmdemo-$DEMO.log}
BIN=${HLBIN:-../host/hlhost}
CMDS=$(mktemp -t hlcmds.XXXXXX); : > "$CMDS"
tail -f "$CMDS" | SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy XASH3D_BASEDIR=$PWD ${HLEXEC:-} "$BIN" -dev 2 -windowed -width 320 -height 240 > "$LOG" 2>&1 &
sleep 5; [ -n "${PRE:-}" ] && echo "$PRE" >> "$CMDS"; echo "timedemo $DEMO" >> "$CMDS"
for i in $(seq 1 3000); do grep -q "timedemo result\|Sys_Error\|Crash" "$LOG" && break; sleep 1; done
echo quit >> "$CMDS"; sleep 2
pkill -f "$BIN"; sleep 1; pkill -9 -f "$BIN"; pkill -f "tail -f $CMDS"; rm -f "$CMDS"
grep "timedemo result\|Sys_Error\|Crash" "$LOG"
