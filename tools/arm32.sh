#!/bin/bash
# 32-bit ARM Linux host build with the allocator of the device (port/host_mem.c HOST_ESP_HEAP):
# the struct sizes, the heap sizes and the fragmentation are those of the ESP32-S3.
# Runs in a podman container (host/arm32/Containerfile), the binary with qemu user mode.
#
#   tools/arm32.sh build [make arguments]     build host/hlhost-arm32
#   tools/arm32.sh run <command...>           run a command in the container with HLBIN/HLEXEC set,
#                                             e.g. tools/arm32.sh run tools/hlrun.sh c0a0
#                                                  tools/arm32.sh run tools/mapsweep.sh
# HLHEAP / HLHEAP_INT change the heap sizes (default: the device ones).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=hl-arm32

if ! podman image exists $IMAGE; then
	podman build -t $IMAGE -f "$ROOT/host/arm32/Containerfile" "$ROOT/host/arm32"
fi

ENVS=()
for v in HLHEAP HLHEAP_INT HLHEAP_MAP HLHEAP_LEVELMAP HLMEM HLMEM_LOG HL_ALLOCLOG HL_ASSETS LOG SWEEP_OUT SWEEP_CMD PRE; do
	[ -n "${!v:-}" ] && ENVS+=( -e "$v=${!v}" )
done

# the ESP-IDF heap sources (host/Makefile IDF_HEAP), from the PlatformIO or IDF_PATH installation
IDF_HEAP=$(ls -d "$HOME"/.platformio/packages/framework-espidf*/components/heap ${IDF_PATH:+"$IDF_PATH/components/heap"} 2>/dev/null | head -1)

run() {
	podman run --rm -i -v "$HOME:$HOME" -w "$ROOT" ${ENVS[@]+"${ENVS[@]}"} \
		-e HLBIN="${HLBIN:-$ROOT/host/hlhost-arm32}" -e HLEXEC=qemu-arm -e IDF_HEAP="$IDF_HEAP" $IMAGE "$@"
}

cmd=${1:-}; shift || true
case "$cmd" in
build)
	run sh -c 'P="PKG_CONFIG_PATH=/usr/lib/arm-linux-gnueabihf/pkgconfig pkg-config"; make -C host -j$(nproc) \
		OBJ=obj-arm32 BIN=hlhost-arm32 ESP_HEAP=1 IDF_HEAP="$IDF_HEAP" OPT="-O2 -g -fno-omit-frame-pointer" \
		CC=arm-linux-gnueabihf-gcc CXX=arm-linux-gnueabihf-g++ LD=arm-linux-gnueabihf-ld OBJCOPY=arm-linux-gnueabihf-objcopy \
		SDL_CFLAGS="$(eval $P --cflags sdl2)" SDL_LIBS="$(eval $P --libs sdl2)" "$@"' make "$@" ;;
run)
	run "$@" ;;
*)
	sed -n '2,12p' "$0"; exit 1 ;;
esac
