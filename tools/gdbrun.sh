#!/bin/sh
# Run a map under the qemu gdb stub and print the crash backtrace (in the container: tools/arm32.sh run tools/gdbrun.sh <map> [gdb commands file])
MAP=$1
HLEXEC="qemu-arm -g 1234" LOG=$PWD/gamedata/run32-gdb.log tools/hlrun.sh $MAP 5 > /dev/null 2>&1 &
sleep 2
gdb-multiarch -batch -ex "set pagination off" -ex "file $HLBIN" -ex "target remote :1234" \
	-ex "handle SIGPIPE SIGUSR1 SIGUSR2 nostop noprint" ${2:+-x "$2"} -ex continue -ex "bt 12" 2>&1 | grep -vE "^\[New|^warning: "
wait
