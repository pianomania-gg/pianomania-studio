/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef MU_IEX_MEI_PMMEIEXPORT_H
#define MU_IEX_MEI_PMMEIEXPORT_H

#include <string>
#include <vector>

namespace mu::engraving {
class Note;
class Score;
}

namespace mu::iex::mei {
struct PmMeiNoteRecord {
    const mu::engraving::Note* note = nullptr;
    std::string scoreEventId;
    std::string chordId;
    std::string measureId;
    std::string beat;
    int idx = 0;
};

//! Headless helper: render an already-laid-out score to MEI (with the pm:
//! pianomania namespace) as a string. Wraps the internal MeiExporter so callers
//! (e.g. the wasm converter) don't need its private pugixml/libmei includes.
bool pmWriteMeiToString(mu::engraving::Score* score, bool exportLayout, std::string& out,
                        std::vector<PmMeiNoteRecord>* noteRecords = nullptr);
}

#endif // MU_IEX_MEI_PMMEIEXPORT_H
