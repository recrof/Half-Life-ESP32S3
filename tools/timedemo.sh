#!/bin/sh
# Device benchmark: plays a demo as fast as possible (timedemo) and prints the frame rate.
# The demo is in valve/ on the SD card (tools/sdput.py); bench.dem walks through c1a0.
#   tools/timedemo.sh [demo] [--flash]
cd "$(dirname "$0")/.."
. tools/idf_env.sh > /dev/null 2>&1
DEMO=${1:-bench}
L=gamedata/esp/console.log
C=gamedata/esp/commands
if [ "$2" = "--flash" ]; then
	tools/flash.sh 2>&1 | grep -E "BUILD FAILED|error" && exit 1
fi
pkill -f serial_daemon.py; sleep 1
: > $L
tools/logger.sh $PWD/gamedata/esp
for i in $(seq 1 60); do grep -qa "Time to first frame" $L && break; sleep 1; done
sleep 2
echo "timedemo $DEMO" >> $C
for i in $(seq 1 400); do grep -qaE "timedemo result|Guru|Xash Error" $L && break; sleep 1; done
grep -aE "timedemo result|Guru|Xash Error|surface cache" $L | tail -3
