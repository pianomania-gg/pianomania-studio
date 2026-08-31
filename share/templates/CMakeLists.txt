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

# adding or renaming templates here should also better be done in convert.json, 
# but definitely need to get reflected in ../instruments/instrumentsxml.h,
# like by running ../instruments/update_instruments_xml.py

# Pianomania: piano-only distribution — ship only the keyboard-relevant
# templates. The full upstream template set stays in the tree for re-forks;
# only the install list is trimmed.
install(FILES
      My_First_Score.mscx
      categories.json
      DESTINATION ${Mscore_SHARE_NAME}${Mscore_INSTALL_NAME}templates
      )
install(DIRECTORY
      01-General/01-Treble_Clef
      01-General/02-Bass_Clef
      01-General/03-Grand_Staff
      DESTINATION ${Mscore_SHARE_NAME}${Mscore_INSTALL_NAME}templates/01-General
      )

install(DIRECTORY
      04-Solo/04-Piano
      DESTINATION ${Mscore_SHARE_NAME}${Mscore_INSTALL_NAME}templates/04-Solo
      )
