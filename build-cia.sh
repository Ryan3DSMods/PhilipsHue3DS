#!/bin/sh
# Builds hue3ds.cia for Home Menu installation.
# Requires: devkitARM (make), bannertool and makerom in PATH.
set -e
make
bannertool makebanner -i ciabuild/banner.png -a ciabuild/banner.wav -o ciabuild/banner.bnr
bannertool makesmdh -s "hue3ds" -l "$(grep APP_DESCRIPTION Makefile | head -1 | cut -d= -f2)" -p "homebrew" -i ciabuild/icon.png -o ciabuild/icon.icn
makerom -f cia -ver 2 -desc app:4 -o hue3ds.cia -elf hue3ds.elf -rsf ciabuild/app.rsf -icon ciabuild/icon.icn -banner ciabuild/banner.bnr -target t
echo "==> hue3ds.cia ready - copy to SD and install with FBI"
