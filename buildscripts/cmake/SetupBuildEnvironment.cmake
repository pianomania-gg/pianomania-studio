# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-Studio-CLA-applies
#
# MuseScore Studio
# Music Composition & Notation
#
# Copyright (C) 2024 MuseScore Limited and others
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

include(GetCompilerInfo)
include(GetBuildType)
include(pianomaniacompatibility.generated)

# Unity build defaults
if(NOT DEFINED CMAKE_UNITY_BUILD_BATCH_SIZE)
    set(CMAKE_UNITY_BUILD_BATCH_SIZE 12)
endif()

# Debug/no-debug options
add_compile_definitions($<$<CONFIG:Debug>:QT_QML_DEBUG>)

add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:NDEBUG>)
add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:QT_NO_DEBUG>)

# String debug hack
if(MUSE_COMPILE_STRING_DEBUG_HACK AND CC_IS_CLANG)
    add_compile_definitions($<$<CONFIG:Debug>:MUSE_STRING_DEBUG_HACK>)
endif()

# Shared libraries
set(BUILD_SHARED_LIBS OFF)
set(SHARED_LIBS_INSTALL_DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)

if(MUSE_COMPILE_USE_SHARED_LIBS_IN_DEBUG AND BUILD_IS_DEBUG)
    if(CC_IS_GCC OR CC_IS_MINGW)
        set(BUILD_SHARED_LIBS ON)
    endif()
endif()

# Address Sanitizer
if(MUSE_COMPILE_ASAN)
    if(CC_IS_CLANG OR CC_IS_GCC OR CC_IS_MINGW)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fsanitize=address>)
        add_compile_options($<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fno-omit-frame-pointer>)
        add_link_options($<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fsanitize=address>)

        add_compile_options($<$<COMPILE_LANGUAGE:Swift>:-sanitize=address>)
        add_link_options($<$<COMPILE_LANGUAGE:Swift>:-sanitize=address>)
    elseif(CC_IS_MSVC)
        add_compile_options("/fsanitize=address")
    endif()
endif()

# Mac-specific
if(OS_IS_MAC)
    set(MACOSX_DEPLOYMENT_TARGET "${PIANOMANIA_DESKTOP_MINIMUM_OS_VERSION}")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${PIANOMANIA_DESKTOP_MINIMUM_OS_VERSION}")
    set(CMAKE_OSX_ARCHITECTURES "${PIANOMANIA_DESKTOP_ARCHITECTURE}")
endif(OS_IS_MAC)

# MSVC-specific
if(CC_IS_MSVC)
    add_compile_options("/EHsc")
    add_compile_options("/utf-8")
    add_compile_options("/MP")
    add_compile_options("/bigobj")

    add_compile_definitions(WIN32 _WINDOWS)
    add_compile_definitions(_UNICODE UNICODE)
    add_compile_definitions(_USE_MATH_DEFINES)
    add_compile_definitions(NOMINMAX)
    add_compile_definitions(_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS)
    
    add_link_options("/NODEFAULTLIB:LIBCMT")
endif()

# MinGW-specific
if(CC_IS_MINGW)
    # https://musescore.org/node/22048
    add_compile_options(-mno-ms-bitfields)

    if(NOT MUSE_COMPILE_BUILD_64)
        add_link_options("-Wl,--large-address-aware")
    endif()

    add_compile_definitions(_UNICODE UNICODE)
endif()

# Wasm-specific
if(CC_IS_EMCC)
    set(EMCC_CMAKE_TOOLCHAIN "" CACHE FILEPATH "Path to EMCC CMake Emscripten.cmake")
    set(EMCC_INCLUDE_PATH "." CACHE PATH "Path to EMCC include dir")

    # NOTE The fonts are no longer embedded here. Engraving carries them as qrc
    # resources and reads them from there, so a second copy on the filesystem was
    # 34MB of the download doing nothing — 18MB of it FontForge .sfd sources that
    # no runtime reads. The copy existed to work around Qt big resources
    # returning garbage sizes on wasm, which is now fixed at the registration
    # (see muse_module_add_qrc), so nothing looks the fonts up by path.

    # Chord-description / style files. The engraving resolves them at runtime from
    # appDataPath()+"/styles" (= "/files/share/styles" in this headless build), e.g.
    # ChordList::read for chords_std.xml. Embed the small (~400KB) styles dir there.
    set(EMCC_EMBED_STYLES_DIR "${CMAKE_SOURCE_DIR}/share/styles" CACHE PATH "Style/chord files embedded into the wasm")

    # Build a loadable JS+wasm library (NOT a .html test page). MODULARIZE exposes a
    # factory named createPmConverter that the webapp calls; FORCE_FILESYSTEM gives
    # us an in-memory MEMFS (no preload, no real FS); ALLOW_MEMORY_GROWTH because
    # large scores need headroom. --bind pulls in Embind for pmConvert().
    set(EMCC_LINK_FLAGS
        "--bind"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createPmConverter"
        "-sEXPORT_ES6=0"
        "-sFORCE_FILESYSTEM=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=134217728"  # 128MB; static data alone is ~43MB, plus score/heap
        "-sMAXIMUM_MEMORY=2147483648"
        "-sEXPORTED_RUNTIME_METHODS=['FS']"
        "-sENVIRONMENT=web,worker,node"  # node enables a headless smoke test
        "--embed-file" "${EMCC_EMBED_STYLES_DIR}@/files/share/styles"
    )
    string(JOIN " " EMCC_LINK_FLAGS_STR ${EMCC_LINK_FLAGS})

    # Output pm-converter.js (+ pm-converter.wasm). No .html, no preload bundle.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/public_html)
    set(CMAKE_EXECUTABLE_SUFFIX ".js")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${EMCC_LINK_FLAGS_STR}")
    set(CMAKE_TOOLCHAIN_FILE ${EMCC_CMAKE_TOOLCHAIN})
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

    if (BUILD_IS_DEBUG)
        set(EMCC_LINKER_FLAGS -O0)
    else()
        set(EMCC_LINKER_FLAGS -Os)
    endif()


endif(CC_IS_EMCC)

# Warnings
include(SetupCompileWarnings)

# User overrides
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/SetupBuildEnvironment.user.cmake")
    include(${CMAKE_CURRENT_LIST_DIR}/SetupBuildEnvironment.user.cmake)
endif()
