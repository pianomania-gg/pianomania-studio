# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-CLA-applies
#
# MuseScore
# Music Composition & Notation
#
# Copyright (C) 2024 MuseScore Limited
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

if (MUE_COMPILE_USE_SYSTEM_HARFBUZZ)
    find_package(HarfBuzz)

    if (HarfBuzz_FOUND)
        message(STATUS "Found HarfBuzz")

        # See HarfBuzz's harfbuzz-config.cmake, which is quite minimalistic
        set(HARFBUZZ_LIBRARIES harfbuzz::harfbuzz)
        set(HARFBUZZ_INCLUDE_DIRS ${HARFBUZZ_INCLUDE_DIR})

        return()
    else()
        message(WARNING "Set MUE_COMPILE_USE_SYSTEM_HARFBUZZ=ON, but system harfbuzz not found, built-in will be used")
    endif()
endif()

# If not MUE_COMPILE_USE_SYSTEM_HARFBUZZ, or if it was not found, build the
# exact historical dependency from its upstream release. The former
# muse_deps/harfbuzz/7.1.0 helper URL no longer exists, so bind the archive and
# checksum directly instead of accepting a moving or silent replacement.
include(FetchContent)
FetchContent_Declare(muse_harfbuzz
    URL https://github.com/harfbuzz/harfbuzz/releases/download/7.1.0/harfbuzz-7.1.0.tar.xz
    URL_HASH SHA256=f135a61cd464c9ed6bc9823764c188f276c3850a8dc904628de2a87966b7077b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(HB_HAVE_FREETYPE ON CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(muse_harfbuzz)

target_no_warning(harfbuzz -Wno-conversion)
target_no_warning(harfbuzz -Wno-unused-parameter)
target_no_warning(harfbuzz -Wno-unused-variable)
target_no_warning(harfbuzz -WMSVC-no-hides-previous)
target_no_warning(harfbuzz -WMSVC-no-unreachable)

#add_subdirectory(thirdparty/msdfgen)

set(HARFBUZZ_LIBRARIES harfbuzz)
set(HARFBUZZ_INCLUDE_DIRS ${muse_harfbuzz_SOURCE_DIR}/src)
