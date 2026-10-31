#!/usr/bin/env bash
# re:Blue - build the PS5 homebrew title (docs/PS5.md).
#
#   ps5/build.sh [--sdk DIR] [--vulkan DIR] [--rexsdk DIR] [--title-id PPSAxxxxx]
#
# Needs: your own Disc 1 default.xex and the .vso/.pso shaders from
# pack/!necessity.ipk in assets/ (docs/PS5.md), the ReXGlue SDK tree with its
# PS5 changes, a PS5 payload SDK and PS5_Vulkan with its RADV release archive
# built, and clang/lld 20 on the host. Writes out/build/ps5/dist/<TITLE_ID>/.
#
# The host steps come first: the SDK's rexglue (codegen) and XenosRecomp
# (game shaders to SPIR-V) run on this machine, never on the console.
#
# REBLUE_PS5_DEBUG_HEAP=ON builds the checking allocator
# (src/platform/ps5_debug_heap.cpp) for chasing heap damage; OFF (the default)
# builds the title as normal. Switching it rebuilds one file.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
parent=$(dirname "$root")
sdk=${PS5_PAYLOAD_SDK:-$parent/PS5_VulkanTemplate/.deps/native/ps5-payload-sdk}
vulkan=${PS5_VULKAN_DIR:-$parent/PS5_Vulkan}
rexsdk=${REXSDK_DIR:-$parent/rexglue-sdk}
title_id=PPSA99213
while (($#)); do
    case $1 in
        --sdk) sdk=$2; shift 2 ;;
        --vulkan) vulkan=$2; shift 2 ;;
        --rexsdk) rexsdk=$2; shift 2 ;;
        --title-id) title_id=$2; shift 2 ;;
        *) echo "usage: ${0##*/} [--sdk DIR] [--vulkan DIR] [--rexsdk DIR] [--title-id PPSAxxxxx]" >&2; exit 2 ;;
    esac
done
[[ -f $sdk/toolchain/prospero.cmake ]] || { echo "no PS5 payload SDK at $sdk" >&2; exit 2; }
[[ -f $vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a ]] ||
    { echo "no RADV release archive in $vulkan (tools/build-radv.sh release)" >&2; exit 2; }
[[ -f $rexsdk/include/rex/platform.h ]] && grep -q REX_PLATFORM_PS5 "$rexsdk/include/rex/platform.h" ||
    { echo "no ReXGlue SDK with the PS5 changes at $rexsdk" >&2; exit 2; }
[[ -f $root/assets/default.xex ]] && compgen -G "$root/assets/*.pso" > /dev/null ||
    { echo "no game files in assets/: default.xex and the .vso/.pso shaders (docs/PS5.md)" >&2; exit 2; }

# Submodule changes the PS5 needs, kept as patches because the submodules
# point at upstream: SDL and FFmpeg in the SDK, plume here. Each is applied
# once; an already-applied patch is left alone.
apply_patch() { # repo-dir patch
    if git -C "$1" apply --check -R "$2" 2> /dev/null; then
        return
    fi
    git -C "$1" apply --check "$2" ||
        { echo "${2##*/} neither applies nor is applied in $1" >&2; exit 1; }
    git -C "$1" apply --whitespace=nowarn "$2"
    echo "applied ${2##*/} in $1"
}
apply_patch "$rexsdk/thirdparty/sdl3" "$rexsdk/tools/patches/sdl-ps5.patch"
apply_patch "$rexsdk/thirdparty/FFmpeg" "$rexsdk/tools/patches/ffmpeg-ps5.patch"
apply_patch "$root/thirdparty/plume" "$root/ps5/patches/plume-ps5.patch"

host=$root/out/build/ps5-host
mkdir -p "$host"

# 1. The SDK's code generator, for this machine.
rexglue=$host/rexglue-sdk/bin/rexglue
if [[ ! -x $rexglue ]]; then
    cmake -S "$rexsdk" -B "$host/rexglue-sdk" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER_TYPE=LLD \
        -DREXGLUE_ENABLE_TRACY=OFF > "$host/rexglue-sdk.configure.log"
    cmake --build "$host/rexglue-sdk" --target rexglue
    mkdir -p "$(dirname "$rexglue")"
    # The SDK writes its binaries to <sdk>/out/<platform>/.
    built=$(find "$rexsdk/out" -maxdepth 2 -type f -name rexglue -perm -u+x | head -1)
    [[ -n $built ]] || { echo "rexglue was not built" >&2; exit 1; }
    ln -sf "$built" "$rexglue"
else
    # Keep it current with the SDK sources (a no-op when nothing changed).
    cmake --build "$host/rexglue-sdk" --target rexglue > "$host/rexglue-sdk.build.log"
fi
# The code templates are compiled into rexglue but are not part of codegen's
# input fingerprint, so a rebuilt generator regenerates the game code.
codegen_stamp=$root/generated/codegen.build.stamp
if [[ -f $codegen_stamp && $(readlink -f "$rexglue") -nt $codegen_stamp ]]; then
    echo "rexglue changed: regenerating the recompiled code"
    rm -f "$codegen_stamp" "$root/generated/codegen.stamp"
fi

# 2. The game shaders, recompiled to SPIR-V by XenosRecomp on this machine.
recomp_src=$root/thirdparty/XenosRecomp
git -C "$recomp_src" submodule update --init --depth 1 --recursive > /dev/null
if [[ ! -x $host/xenosrecomp/XenosRecomp/XenosRecomp ]]; then
    cmake -S "$recomp_src" -B "$host/xenosrecomp" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-DREBLUE_RECOMP \
        > "$host/xenosrecomp.configure.log"
    cmake --build "$host/xenosrecomp" --target XenosRecomp
fi
shader_cache=$host/shader_cache.cpp
if [[ ! -f $shader_cache || -n $(find "$root/assets" -newer "$shader_cache" -print -quit) ]]; then
    "$host/xenosrecomp/XenosRecomp/XenosRecomp" "$root/assets" "$shader_cache.new" \
        "$recomp_src/XenosRecomp/shader_common.h" > "$host/xenosrecomp.log"
    mv "$shader_cache.new" "$shader_cache"
fi

# The host shaders (src/gpu/shaders/hlsl) go through XenosRecomp's Linux dxc.
dxc_bin=$recomp_src/thirdparty/dxc-bin
dxc="LD_LIBRARY_PATH=$dxc_bin/lib/x64;$dxc_bin/bin/x64/dxc-linux"
chmod +x "$dxc_bin/bin/x64/dxc-linux"

# 3. The title.
build=$root/out/build/ps5
configure() {
    cmake -S "$root" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
        -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DREXSDK_DIR="$rexsdk" -DREXGLUE_HOST_TOOL="$rexglue" \
        -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF \
        -DREBLUE_PS5_SHADER_CACHE="$shader_cache" -DREBLUE_DXC="$dxc" \
        -DREBLUE_PS5_VULKAN_DIR="$vulkan" -DREBLUE_PS5_TITLE_ID="$title_id" \
        -DREBLUE_PS5_DEBUG_HEAP="${REBLUE_PS5_DEBUG_HEAP:-OFF}"
}
configure
# Codegen writes generated/sources.cmake; configure again to compile it.
cmake --build "$build" --target reblue_codegen
configure
cmake --build "$build" --target reblue ${BUILD_JOBS:+-j "$BUILD_JOBS"}
echo "title folder: $build/dist/$title_id"
echo "copy it to /data/homebrew/$title_id/, and the extracted game files to its game/ folder (docs/PS5.md)"
