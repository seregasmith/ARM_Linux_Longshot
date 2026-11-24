#!/bin/sh
# Builds the setup window, builder/linux/setup_ui (the ARM64 setup AppImage's longshot-setup, in its usr/bin):
#
#   builder/linux/setup_ui/build.sh <out> <SDL2 include folder> <libSDL2-2.0.so.0> <Vulkan include folder>
#
# CC and CXX are the compilers (default clang and clang++). CXXFLAGS holds the target and sysroot flags for both.
# Dear ImGui is third_party/imgui with its SDL2 and SDL_Renderer backends. SDL2 is the AppImage's own: the program finds it
# at ../lib/longshot/sdl2 beside itself (the AppImage's usr/lib/longshot/sdl2), and the C++ runtime is linked in.
set -eu
if [ $# -ne 4 ]; then
    echo "usage: $0 <out> <SDL2 include folder> <libSDL2-2.0.so.0> <Vulkan include folder>" >&2
    exit 2
fi
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../../.." && pwd)
IMGUI=$ROOT/third_party/imgui
mkdir -p "$(dirname -- "$1")"
# shellcheck disable=SC2086
"${CXX:-clang++}" -std=c++20 -O2 -g0 -Wall -Wno-unused-function ${CXXFLAGS:-} \
    -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS -DSDL_MAIN_HANDLED \
    -I"$HERE" -I"$IMGUI" -I"$IMGUI/backends" -I"$2" \
    "$HERE/setup_ui.cpp" \
    "$IMGUI/imgui.cpp" "$IMGUI/imgui_draw.cpp" "$IMGUI/imgui_tables.cpp" "$IMGUI/imgui_widgets.cpp" \
    "$IMGUI/backends/imgui_impl_sdl2.cpp" "$IMGUI/backends/imgui_impl_sdlrenderer2.cpp" \
    "$3" -Wl,-rpath,'$ORIGIN/../lib/longshot/sdl2' -static-libstdc++ -static-libgcc -lpthread \
    -o "$1"
# shellcheck disable=SC2086
"${CC:-clang}" -std=c11 -O2 -g0 -Wall ${CXXFLAGS:-} -I"$4" \
    "$HERE/steam_driver_probe.c" -ldl -o "$(dirname -- "$1")/longshot-vulkan-driver"
echo "built $1"
