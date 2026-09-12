#!/bin/sh
# Build snesref on Linux/macOS. Windows uses build.bat (MSVC + the SDL2 VC
# dev package); everything else needs SDL2 development headers on the system.
#
#   ./build.sh && ./snesref /path/to/snes9x_libretro.so game.sfc
#
# The libretro core is supplied by you and licensed separately: any SNES core
# with the standard entry points works (snes9x, bsnes, …). On Linux a
# RetroArch install usually has one under ~/.config/retroarch/cores or, for
# the Flatpak, ~/.var/app/org.libretro.RetroArch/config/retroarch/cores.
set -e
cd "$(dirname "$0")"
CXX="${CXX:-c++}"
$CXX -std=c++11 -O2 -o snesref frontend.cpp $(sdl2-config --cflags --libs) -ldl
echo "built ./snesref"
