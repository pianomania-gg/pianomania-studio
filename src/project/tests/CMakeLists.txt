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

set(MODULE_TEST project_test)

set(MODULE_TEST_SRC
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.h
    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mocks/projectconfigurationmock.h
    ${CMAKE_CURRENT_LIST_DIR}/mscmetareadertests.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pianomaniaexporttest.cpp
    ${CMAKE_CURRENT_LIST_DIR}/templatesrepositorytest.cpp
)

# Pianomania export tests read fixtures across modules (project + importexport/midi),
# so the data root is the repo root.
set(MODULE_TEST_DATA_ROOT ${PROJECT_SOURCE_DIR})

set(MODULE_TEST_LINK
    engraving
    iex_mei
    iex_midi
    project
)

include(SetupGTest)


# GTest exits 0 when a filter or build change silently removes tests. Require the
# exact expected count so the pianomania coordinated-export suite cannot vanish
# without failing this gate. Update the count when tests are added deliberately.
set_tests_properties(project_test PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[  PASSED  \\] 37 tests"
)
