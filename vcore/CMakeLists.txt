# SPDX-FileCopyrightText: Copyright 2026 Dreams-Recomp contributors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# vcore: the Dreams port's GPU library (libvcore.so). It implements the bbport 0.7 runtime<->GPU
# C API (port/gpu/bbgpu.h) on top of the *unmodified-in-place* shadPS4 GPU sources of
# ref/shadps4 branch `vcore` (= branch `dreams`: latest upstream + Dreams-ShadPS4 fork patches,
# plus a few `// vcore:` edits). See port/docs/dreams/VCORE.md.
#
# Layout:
#   ${SHADPS4_DIR}/src/...         compiled in place (video_core, shader_recompiler, common subset,
#                                  core/libraries/{gnmdriver,videoout,kernel/equeue}, settings)
#   shim/include/...               headers that SHADOW shadPS4 core headers (searched first)
#   shim/*.cpp                     glue: bbgpu C API, MemoryManager over runtime_memory_*, signals,
#                                  logging, window, kernel/system-service bits, symbol table
cmake_minimum_required(VERSION 3.24)
project(vcore LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2) walkable.
add_compile_options(-fno-omit-frame-pointer)
# Same ISA level as the reference shadPS4 build (ref/shadps4/build: -march=x86-64-v3).
set(VCORE_MARCH "x86-64-v3" CACHE STRING "-march for vcore and its static deps")
if (VCORE_MARCH)
    add_compile_options(-march=${VCORE_MARCH})
endif()

get_filename_component(VCORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SHADPS4_DIR "${VCORE_ROOT}/ref/shadps4" CACHE PATH "shadPS4 tree (branch vcore)")
set(SRC ${SHADPS4_DIR}/src)
set(EXT ${SHADPS4_DIR}/externals)
if (NOT EXISTS ${SRC}/video_core/amdgpu/liverpool.cpp)
    message(FATAL_ERROR "SHADPS4_DIR=${SHADPS4_DIR} does not look like a shadPS4 tree")
endif()

# ---------------------------------------------------------------------------------------------
# Third-party: shadPS4's own pinned externals (same versions the reference emulator built with).
# SDL3 and Vulkan loader come from the system (the loader/runtime links the same SDL3).
# ---------------------------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
add_subdirectory(${EXT}/ext-boost ext/boost EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/fmt ext/fmt EXCLUDE_FROM_ALL)
set(VULKAN_HEADERS_ENABLE_MODULE OFF)
add_subdirectory(${EXT}/vulkan-headers ext/vulkan-headers EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/vma ext/vma EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/robin-map ext/robin-map EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/xbyak ext/xbyak EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/magic_enum ext/magic_enum EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/toml11 ext/toml11 EXCLUDE_FROM_ALL)
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(${EXT}/json ext/json EXCLUDE_FROM_ALL)
add_library(vcore_xxhash STATIC ${EXT}/xxhash/xxhash.c)
target_include_directories(vcore_xxhash PUBLIC ${EXT}/xxhash)
option(ZYDIS_BUILD_TOOLS "" OFF)
option(ZYDIS_BUILD_EXAMPLES "" OFF)
add_subdirectory(${EXT}/zydis ext/zydis EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/sirit ext/sirit EXCLUDE_FROM_ALL)
add_subdirectory(${EXT}/miniz ext/miniz EXCLUDE_FROM_ALL)
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_ENABLE_INSTALL OFF)
add_subdirectory(${EXT}/abseil-cpp ext/abseil EXCLUDE_FROM_ALL)
find_package(Vulkan REQUIRED)                 # system loader (libvulkan.so)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

# Host shaders: shadPS4's own CMake file, run with the system glslang.
add_subdirectory(${SRC}/video_core/host_shaders host_shaders)

# scm_rev.cpp from shadPS4's template (version strings only).
set(APP_VERSION "vcore")
set(APP_IS_RELEASE false)
execute_process(COMMAND git -C ${SHADPS4_DIR} rev-parse --short HEAD OUTPUT_VARIABLE GIT_REV
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C ${SHADPS4_DIR} rev-parse --abbrev-ref HEAD OUTPUT_VARIABLE GIT_BRANCH
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(GIT_DESC "${GIT_BRANCH}-${GIT_REV}")
set(GIT_REMOTE_NAME "origin")
set(GIT_REMOTE_URL "https://github.com/shadps4-emu/shadPS4")
string(TIMESTAMP BUILD_DATE "%Y-%m-%d")
configure_file(${SRC}/common/scm_rev.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/scm_rev.cpp @ONLY)

# ---------------------------------------------------------------------------------------------
# shadPS4 sources compiled in place.
# ---------------------------------------------------------------------------------------------
file(GLOB_RECURSE VCORE_VIDEO_CORE CONFIGURE_DEPENDS ${SRC}/video_core/*.cpp)
file(GLOB_RECURSE VCORE_RECOMPILER CONFIGURE_DEPENDS ${SRC}/shader_recompiler/*.cpp)
# renderdoc.cpp: replaced by shim/renderdoc_stub.cpp (no in-process RenderDoc in the native port).
list(FILTER VCORE_VIDEO_CORE EXCLUDE REGEX "/video_core/renderdoc\\.cpp$")
# Offline replay tool sources (fork tools/, not part of the renderer) if any live under video_core.
list(FILTER VCORE_VIDEO_CORE EXCLUDE REGEX "/host_shaders/")
set(VCORE_COMMON
    ${SRC}/common/logging/classes.cpp
    ${SRC}/common/decoder.cpp
    ${SRC}/common/error.cpp
    ${SRC}/common/io_file.cpp
    ${SRC}/common/native_clock.cpp
    ${SRC}/common/number_utils.cpp
    ${SRC}/common/path_util.cpp
    ${SRC}/common/rdtsc.cpp
    ${SRC}/common/recursive_lock.cpp
    ${SRC}/common/signal_context.cpp
    ${SRC}/common/slot_vector.cpp
    ${SRC}/common/spin_lock.cpp
    ${SRC}/common/string_util.cpp
    ${SRC}/common/stb.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/scm_rev.cpp)
set(VCORE_CORE
    ${SRC}/core/emulator_settings.cpp
    ${SRC}/core/emulator_state.cpp
    ${SRC}/core/libraries/gnmdriver/gnmdriver.cpp
    ${SRC}/core/libraries/videoout/driver.cpp
    ${SRC}/core/libraries/videoout/video_out.cpp
    ${SRC}/core/libraries/kernel/equeue.cpp)
file(GLOB VCORE_SHIM CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shim/*.cpp)
# bbport 0.7's native AvPlayer (FFmpeg), compiled in place from the port's own tree: it already
# talks to the native runtime (runtime_file_translate, runtime_thread_attach_host).
# Interim: the runtime is getting its own CPU-side AvPlayer; the loader asks the runtime before
# bbgpu_resolve(), so a runtime implementation overrides this one automatically. Turn it off with
# -DVCORE_BBPORT_AVPLAYER=OFF once the runtime has it.
option(VCORE_BBPORT_AVPLAYER "Serve libSceAvPlayer with bbport's AvPlayer (interim)" ON)
set(VCORE_BBPORT_AVPLAYER_SOURCES "")
if (VCORE_BBPORT_AVPLAYER)
    set(VCORE_BBPORT_AVPLAYER_SOURCES
        ${VCORE_ROOT}/port/gpu/platform/avplayer/avplayer.cpp
        ${VCORE_ROOT}/port/gpu/platform/avplayer/player.cpp
        ${VCORE_ROOT}/port/gpu/platform/avplayer/source.cpp)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale
                      libswresample)
endif()

add_library(vcore SHARED ${VCORE_VIDEO_CORE} ${VCORE_RECOMPILER} ${VCORE_COMMON} ${VCORE_CORE}
            ${VCORE_SHIM} ${VCORE_BBPORT_AVPLAYER_SOURCES})
add_dependencies(vcore host_shaders)
# Include order matters: shim/include shadows shadPS4's core headers (core/memory.h, sdl_window.h,
# core/libraries/libs.h, imgui/..., ...). Quoted includes are looked up next to the including file
# first, then here, in order.
target_include_directories(vcore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shim/include
    ${SRC}
    ${SHADPS4_DIR}
    ${HOST_SHADERS_INCLUDE}
    ${VCORE_ROOT}/port                        # gpu/bbgpu.h (the C API we implement)
    ${VCORE_ROOT}/port/gpu)                   # bbport AvPlayer: platform/..., shim/bb_env.h (last:
                                              # its common/, core/ never shadow shadPS4's)
target_include_directories(vcore SYSTEM PRIVATE
    ${EXT}/gcn/include ${EXT}/half/include ${EXT}/tracy/public ${EXT}/renderdoc ${EXT}/stb)
target_compile_definitions(vcore PRIVATE
    VCORE_NATIVE=1 BOOST_ASIO_STANDALONE ZYCORE_STATIC_BUILD ZYDIS_STATIC_BUILD MINIZ_STATIC_DEFINE
    TRACY_NO_CALLSTACK)
target_compile_options(vcore PRIVATE
    -Wno-invalid-offsetof -Wno-deprecated-declarations -Wno-unknown-warning-option
    # The game's threads call into this library; glibc's TLS via fs must stay initial-exec
    # (same as bbport/Lantern's GPU library).
    -ftls-model=initial-exec)
target_link_libraries(vcore PRIVATE
    Boost::headers fmt::fmt Vulkan::Headers GPUOpen::VulkanMemoryAllocator tsl::robin_map
    xbyak::xbyak magic_enum::magic_enum toml11::toml11 nlohmann_json::nlohmann_json vcore_xxhash
    Zydis::Zydis sirit miniz absl::flat_hash_map absl::flat_hash_set absl::hash
    PkgConfig::SDL3 ${CMAKE_DL_LIBS} pthread)
if (VCORE_BBPORT_AVPLAYER)
    target_link_libraries(vcore PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(vcore PRIVATE VCORE_BBPORT_AVPLAYER=1)
endif()
# runtime_* symbols (runtime_memory_*, runtime_process_time_*, ...) resolve against the loader.
target_link_options(vcore PRIVATE -Wl,--allow-shlib-undefined -Wl,-z,undefs)
set_target_properties(vcore PROPERTIES OUTPUT_NAME vcore)
