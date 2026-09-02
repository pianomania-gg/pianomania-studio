# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-Studio-CLA-applies
#
# MuseScore Studio
# Music Composition & Notation
#
# Copyright (C) 2021 MuseScore Limited
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

# Headless WebAssembly converter: exposes pmConvert() (Embind) which turns a
# MuseScore file into our custom MEI + MIDI bundle, fully in-memory. Built only
# for the wasm target (see src/CMakeLists.txt, OS_IS_WASM branch).
#
# It operates at the engraving level (EngravingProject/MscReader to load,
# MeiExporter to write MEI, and pianomaniaexport.cpp — compiled in here — for
# MIDI), deliberately bypassing the editor-coupled project/notation modules.

muse_create_module(pmwasm)

target_sources(pmwasm PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/pmconverter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pmconverter.h

    # Pianomania MIDI export lives in the project module but depends only on
    # engraving + importexport/midi, so we compile it directly here to avoid
    # building the (editor-coupled) project module for wasm.
    ${CMAKE_CURRENT_LIST_DIR}/../project/internal/pianomaniaexport.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../project/internal/pianomaniaexport.h
    )

target_link_libraries(pmwasm PRIVATE
    engraving
    iex_mei
    iex_midi
    )

# NOTE: the app target (src/app/CMakeLists.txt, CC_IS_EMSCRIPTEN branch) embeds
# pmwasm's objects via $<TARGET_OBJECTS:pmwasm>, which keeps the Embind
# EMSCRIPTEN_BINDINGS static initializers in pmconverter.cpp alive at link.
# muse_create_module produces the same kind of target as every other module in
# that loop, so the objects are collected the same way they always were.
