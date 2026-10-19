#!/bin/sh
# After cloning: fetch the engine and game sources and the parts of the engine's
# third party code the build uses, then apply this port's changes to them.
set -e
cd "$(dirname "$0")/.."
git submodule update --init xash3d-fwgs hlsdk-portable
git -C hlsdk-portable submodule update --init freevgui
cd xash3d-fwgs
git submodule update --init 3rdparty/library_suffix 3rdparty/mainui 3rdparty/MultiEmulator 3rdparty/bzip2/bzip2
git -C 3rdparty/mainui submodule update --init
apply() { # repository patch
	if git -C "$1" apply --check "$2" 2>/dev/null; then
		git -C "$1" apply "$2"
		echo "applied $(basename "$2")"
	elif git -C "$1" apply --check --reverse "$2" 2>/dev/null; then
		echo "already applied $(basename "$2")"
	else
		echo "cannot apply $(basename "$2") to $1"
		exit 1
	fi
}
P="$(cd ../patches && pwd)"
apply 3rdparty/library_suffix "$P/xash3d-fwgs-3rdparty-library_suffix.patch"
apply 3rdparty/mainui "$P/xash3d-fwgs-3rdparty-mainui.patch"
apply 3rdparty/mainui/miniutl "$P/xash3d-fwgs-3rdparty-mainui-miniutl.patch"
