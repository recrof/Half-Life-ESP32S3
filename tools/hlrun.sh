#!/bin/bash
# Run the host build on a map, wait until the player is in the game, run commands and quit.
#
#   tools/hlrun.sh <map> [seconds in game] [console commands...]
#
# HLMEM=<size> sets the memory budget, HLBIN the binary, HLEXEC a command that runs it (the
# 32-bit build: tools/arm32.sh sets them), LOG the log file (default
# gamedata/run-<map>.log). Prints the memory report and the errors.
set -u
cd "$(dirname "$0")/../gamedata"
MAP=$1; WAIT=${2:-3}; shift; shift 2>/dev/null
BIN=${HLBIN:-../host/hlhost}
LOG=${LOG:-run-$MAP.log}
CMDS=$(mktemp -t hlcmds.XXXXXX)
: > "$CMDS"

tail -f "$CMDS" | SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-dummy} SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy} SDL_VIDEO_HIGHDPI_DISABLED=1 XASH3D_BASEDIR=$PWD ${HLEXEC:-} "$BIN" -dev 2 -windowed \
	-width 320 -height 240 +map "$MAP" > "$LOG" 2>&1 &
PIPE_PID=$!

# wait for the player to spawn (or the game to fail)
for i in $(seq 1 600); do
	grep -q "client connected" "$LOG" && break
	pgrep -f "$BIN" > /dev/null || break
	sleep 0.1
done
sleep "$WAIT"
for c in "$@"; do echo "$c" >> "$CMDS"; sleep 0.5; done
echo hlmem >> "$CMDS"; sleep 0.5
echo quit >> "$CMDS"
for i in $(seq 1 50); do pgrep -f "$BIN" > /dev/null || break; sleep 0.1; done
pkill -f "$BIN" 2>/dev/null; sleep 1; pkill -9 -f "$BIN" 2>/dev/null
kill $PIPE_PID 2>/dev/null
pkill -f "tail -f $CMDS" 2>/dev/null
rm -f "$CMDS"

grep -q "client connected" "$LOG" && echo "$MAP: loaded" || echo "$MAP: NOT LOADED"
grep -E "HLMEM|Error|error:|Sys_Error|Crash|out of memory" "$LOG" | grep -v "Could not load sound\|couldn't exec" | head -8
