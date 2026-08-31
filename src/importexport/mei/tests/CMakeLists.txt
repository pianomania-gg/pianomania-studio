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

set(MODULE_TEST iex_mei_tests)

set(MODULE_TEST_SRC
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorerw.h
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorecomp.cpp
    ${PROJECT_SOURCE_DIR}/src/engraving/tests/utils/scorecomp.h

    ${CMAKE_CURRENT_LIST_DIR}/environment.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mei_tests.cpp
)

set(MODULE_TEST_LINK
    engraving
    iex_mei
)

set(MODULE_TEST_DATA_ROOT ${CMAKE_CURRENT_LIST_DIR})

include(SetupGTest)

string(CONCAT PIANOMANIA_MEI_TEST_FILTER
    "Mei_Tests.mei_hairpin_export_includes_pm_hairpin_lines_when_endpoints_present:"
    "Mei_Tests.mei_line_hairpin_exports_resolved_center_and_layout_y_offset:"
    "Mei_Tests.mei_directives_export_resolved_center_between_staves:"
    "Mei_Tests.mei_export_omits_invisible_note_and_idx:"
    "Mei_Tests.mei_export_rehearsal_mark_on_hidden_rest:"
    "Mei_Tests.mei_pianomania_grace_same_pitch_indices:"
    "Mei_Tests.mei_export_laissez_vibrer_keeps_pm_geometry:"
    "Mei_Tests.mei_export_unresolved_pedal_endpoint_omits_pm_geometry:"
    "Mei_Tests.mei_export_connected_pedal_keeps_pm_geometry:"
    "Mei_Tests.mei_export_pianomania_rubato_zone:"
    "Mei_Tests.mei_export_pianomania_470_properties_survive_mscx_round_trip:"
    "Mei_Tests.mei_export_pianomania_rubato_zone_resolves_hidden_boundaries_to_one_anchor:"
    "Mei_Tests.mei_export_beam_and_tuplet_boundaries_ignore_omitted_hidden_chord:"
    "Mei_Tests.mei_export_tuplets_include_complete_resolved_geometry:"
    "Mei_Tests.mei_export_spanners_on_hidden_rests_use_visible_same_staff_notes:"
    "Mei_Tests.mei_export_pianomania_rubato_zone_overlap_fails:"
    "Mei_Tests.mei_export_pianomania_pyro_span_free_range:"
    "Mei_Tests.mei_export_pianomania_laser_span_coexists_with_rubato_zone:"
    "Mei_Tests.mei_export_pianomania_pyro_span_overlap_fails"
)

add_test(
    NAME iex_mei_pianomania_tests
    COMMAND iex_mei_tests
        --gtest_filter=${PIANOMANIA_MEI_TEST_FILTER}
)

# GTest exits 0 when a filter matches nothing, so a gate whose tests were dropped
# would report success while running zero tests. Require the exact expected count.
set_tests_properties(iex_mei_pianomania_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[  PASSED  \\] 19 tests"
)
