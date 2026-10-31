# cmake/RebluePs5.cmake
# PlayStation 5 homebrew title build, with the PS5 payload SDK's toolchain
# (toolchain/prospero.cmake) and Mihawk's PS5_Vulkan (RADV linked into the
# title). Modelled on the Lost Odyssey PS5 port. See docs/PS5.md.
#
#   REBLUE_PS5_VULKAN_DIR       PS5_Vulkan checkout with its RADV release archive
#   REBLUE_PS5_TITLE_ID         PPSAxxxxx title id written to sce_sys/param.json
#   REBLUE_PS5_SHADER_CACHE     shader_cache.cpp from a host XenosRecomp run over
#                               assets/ (ps5/build.sh makes it): the recompiler is
#                               a host tool and is not built for the console
#   REXSDK_DIR                  the ReXGlue SDK tree with its PS5 changes
#   REXGLUE_HOST_TOOL           host rexglue from that tree (runs codegen)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Nothing the desktop builds stage beside the exe exists on the console.
set(REBLUE_D3D12 OFF CACHE BOOL "" FORCE)
set(REBLUE_VULKAN_EXE OFF CACHE BOOL "" FORCE)
# Copy extracted game files to the title folder over FTP instead (docs/PS5.md).
set(REBLUE_BUILD_INSTALLER OFF CACHE BOOL "" FORCE)
set(REBLUE_PROFILING OFF CACHE BOOL "" FORCE)
set(REBLUE_PSO_CAP OFF CACHE BOOL "" FORCE)

# The console's CPU is an 8-core Zen 2.
add_compile_options(-march=znver2)

get_filename_component(_reblue_ps5_parent "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
set(REBLUE_PS5_VULKAN_DIR "${_reblue_ps5_parent}/PS5_Vulkan" CACHE PATH "PS5_Vulkan checkout (RADV for the console)")
set(REBLUE_PS5_TITLE_ID "PPSA99213" CACHE STRING "Title id of the PS5 homebrew title")
set(REBLUE_PS5_TITLE_NAME "Blue Dragon" CACHE STRING "Home screen name of the PS5 homebrew title")
set(REBLUE_PS5_DIST_DIR "${CMAKE_BINARY_DIR}/dist" CACHE PATH "Where the title folder <TITLE_ID>/ is written")
set(REBLUE_PS5_SHADER_CACHE "" CACHE FILEPATH "Host-generated shader_cache.cpp (ps5/build.sh)")
if(NOT REBLUE_PS5_SHADER_CACHE OR NOT EXISTS "${REBLUE_PS5_SHADER_CACHE}")
    message(FATAL_ERROR "PS5 build: set REBLUE_PS5_SHADER_CACHE to the shader_cache.cpp that a "
        "host XenosRecomp makes from assets/ (ps5/build.sh does both).")
endif()

if(NOT EXISTS "${REBLUE_PS5_VULKAN_DIR}/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a")
    message(WARNING "No RADV release archive in ${REBLUE_PS5_VULKAN_DIR}; the title link will fail. "
        "Build it with PS5_Vulkan's tools/build-radv.sh release (docs/PS5.md).")
endif()

# Every executable is linked as a PS5 title by ps5/link-title.sh, which gets
# CMake's objects and fully resolved static libraries and runs PS5_Vulkan's
# link recipe (RADV, the SDK's platform layer, the CRT, the fake SELF).
set(_reblue_ps5_link_config "${CMAKE_BINARY_DIR}/ps5-link.conf")
file(WRITE "${_reblue_ps5_link_config}"
    "vulkan='${REBLUE_PS5_VULKAN_DIR}'\n"
    "sdk='${PS5_PAYLOAD_SDK}'\n"
    "title_id='${REBLUE_PS5_TITLE_ID}'\n"
    "title_name='${REBLUE_PS5_TITLE_NAME}'\n"
    "app_version='${PROJECT_VERSION}'\n"
    "dist='${REBLUE_PS5_DIST_DIR}'\n"
    "param_template='${CMAKE_SOURCE_DIR}/ps5/sce_sys/param.json'\n"
    "icon='${CMAKE_SOURCE_DIR}/ps5/sce_sys/icon0.png'\n"
    "env_example='${CMAKE_SOURCE_DIR}/ps5/ps5-env.example.txt'\n"
    "work='${CMAKE_BINARY_DIR}/ps5-link'\n")
set(CMAKE_CXX_LINK_EXECUTABLE
    "bash \"${CMAKE_SOURCE_DIR}/ps5/link-title.sh\" \"${_reblue_ps5_link_config}\" <TARGET> <OBJECTS> <LINK_LIBRARIES>")
set(CMAKE_C_LINK_EXECUTABLE "${CMAKE_CXX_LINK_EXECUTABLE}")
