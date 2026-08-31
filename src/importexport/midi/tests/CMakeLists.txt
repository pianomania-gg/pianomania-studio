# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-Studio-CLA-applies
#
# MuseScore Studio
# Music Composition & Notation
#
# Copyright (C) 2021 MuseScore Limited and others
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

set(MODULE_TEST iex_midi_tests)

set(MODULE_TEST_SRC
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.h
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorecomp.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorecomp.h

    ${CMAKE_CURRENT_LIST_DIR}/utils/smfyamlserializer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/utils/smfyamlserializer.h

    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/midiimport_tests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/midiexport_tests.cpp
)

set(MODULE_TEST_LINK
    engraving
    iex_midi
    )

set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)

# Pianomania owns midiexport_timing_tests.cpp, which is QTest-based and carries its
# own main, so it builds as a separate target from the upstream GTest suite.
set(MODULE_TEST iex_midi_pianomania_tests)

set(MODULE_TEST_SRC
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.h

    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/testbase.cpp
    ${CMAKE_CURRENT_LIST_DIR}/testbase.h
    ${CMAKE_CURRENT_LIST_DIR}/midiexport_timing_tests.cpp
)

set(MODULE_TEST_LINK
    engraving
    iex_midi
    )

set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(${PROJECT_SOURCE_DIR}/src/framework/testing/qtest.cmake)

# environment.cpp and midiexport_timing_tests.cpp are shared with the GTest target
# and reference its data-root macro.
target_compile_definitions(iex_midi_pianomania_tests PRIVATE
    iex_midi_tests_DATA_ROOT="${CMAKE_CURRENT_LIST_DIR}"
)

# QTest exits 0 only on success, but a build that silently dropped the Pianomania
# tests would still pass with a smaller count. Require the exact expected total.
set_tests_properties(iex_midi_pianomania_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "Totals: 26 passed, 0 failed"
)
