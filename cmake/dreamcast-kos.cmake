# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Samuele Voltan

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(PLATFORM_DREAMCAST TRUE)
set(LBA2_TARGET_DREAMCAST TRUE)

set(CMAKE_CROSSCOMPILING TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER "kos-cc")
set(CMAKE_CXX_COMPILER "kos-c++")
set(CMAKE_AR "kos-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB "kos-ranlib" CACHE FILEPATH "Ranlib")
set(CMAKE_ASM_COMPILER "kos-as")
set(CMAKE_LINKER "kos-c++")

set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_SYSTEM_INCLUDE_PATH "$ENV{KOS_BASE}/include")

set(CMAKE_EXECUTABLE_SUFFIX ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")

add_definitions(
    -D__DREAMCAST__
    -DDREAMCAST
    -D_arch_dreamcast
    -D__arch_dreamcast
    -D_arch_sub_pristine
    -DLBA2_TARGET_DREAMCAST
)

if(NOT CMAKE_BUILD_TYPE MATCHES Debug)
    add_definitions(-DNDEBUG)
endif()

# LBA2_DEBUG_PERF gates Dreamcast boot-trace breadcrumbs ([DC-TRACE]),
# per-frame video timing ([VID-PERF]), libsmacker microsecond counters,
# and once-per-stream KOS-AICA info logs. Off by default; pass
# -DLBA2_DEBUG_PERF=ON at configure time for UART-side diagnostics on
# real hardware.
if(LBA2_DEBUG_PERF)
    add_definitions(-DLBA2_DEBUG_PERF)
endif()

set(CMAKE_ASM_FLAGS "")
set(CMAKE_ASM_FLAGS_RELEASE "")

# The codebase is written in C++98 style. Forced to C++14 here only for KOS
# headers; relax narrowing checks (legitimate U16/S16 reinterprets in SINTAB
# and friends) and other C++98-vs-C++14 strictness with -fpermissive.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-narrowing -fpermissive")

# LBA2 backend defaults appropriate for Dreamcast V2.6
# MVIDEO_BACKEND=smacker enables libsmacker FMV. Audio side goes through
# AIL/KOS/VIDEO_AUDIO_KOS.CPP (STREAM_KOS push channel); video frames blit
# through the existing 8bpp Log + palette path that already runs at 60fps.
set(SOUND_BACKEND "kos"      CACHE STRING "Sound backend (null|miles|sdl|kos)" FORCE)
set(MVIDEO_BACKEND "smacker" CACHE STRING "Motion video backend (null|smacker)" FORCE)
set(ENABLE_ASM    OFF        CACHE BOOL   "x86 ASM disabled on SH4" FORCE)

# DC has libm; libpthread exists in KOS but LBA2 is single-threaded so
# we don't link it by default. Add it explicitly later if anything needs it.
link_libraries(m)
