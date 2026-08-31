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

# Pianomania: English-only distribution. The full upstream set is 210 .qm
# files (~46MB installed); English alone is what our users see.
file(GLOB TS_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*_en.ts
    ${CMAKE_CURRENT_LIST_DIR}/*_en_??.ts
)

if (MUE_RUN_LRELEASE)
    add_custom_target(translations ALL
        COMMENT "Generating .qm files" VERBATIM
    )

    qt_add_lrelease(translations TS_FILES ${TS_FILES} NO_GLOBAL_TARGET QM_FILES_OUTPUT_VARIABLE QM_FILES)
endif()

install(FILES
    ${CMAKE_CURRENT_LIST_DIR}/languages.json
    ${QM_FILES}
    DESTINATION ${Mscore_SHARE_NAME}${Mscore_INSTALL_NAME}locale
)

# Pianomania: Qt's own translations are only needed for non-English UI
# languages, which this distribution does not ship.
