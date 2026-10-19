#!/bin/sh
# Device FPS benchmark: flash, load a map, pause the game at the same view, measure the frame
# rate, profile the game core and take a screenshot.   tools/bench.sh [map] [seconds before the pause]
cd "$(dirname "$0")/.."
. tools/idf_env.sh > /dev/null 2>&1
MAP=${1:-c0a0e}; WAIT=${2:-20}
L=gamedata/esp/console.log
C=gamedata/esp/commands
tools/flash.sh 2>&1 | grep -E "BUILD FAILED|error" && exit 1
: > $L
tools/logger.sh $PWD/gamedata/esp
for i in $(seq 1 60); do grep -qa "Time to first frame" $L && break; sleep 1; done
sleep 2
echo "map $MAP" >> $C
for i in $(seq 1 150); do grep -qaE "client connected|Guru|Xash Error" $L && break; sleep 1; done
sleep $WAIT
echo pause >> $C
sleep 5
echo fps >> $C; sleep 10; echo fps >> $C; sleep 10; echo fps >> $C
echo "prof 5000" >> $C; sleep 12
echo shot >> $C; sleep 10
grep -aE "fps:|Guru|Xash Error|connected|surface cache" $L | tail -6
python3 tools/profile.py $L build/HalfLifeESP32S3.elf 20
