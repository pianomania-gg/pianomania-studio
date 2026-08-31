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
#include "../pmmeiexport.h"

#include "engraving/compat/midi/compatmidirender.h"

#include "meiexporter.h"

namespace mu::iex::mei {
bool pmWriteMeiToString(mu::engraving::Score* score, bool exportLayout, std::string& out,
                        std::vector<PmMeiNoteRecord>* noteRecords)
{
    // Canonical note indices must follow the same authored playback events as
    // coordinated MIDI, including after-graces scheduled inside their parent
    // chord duration.
    mu::engraving::CompatMidiRender::createPlayEvents(score);

    MeiExporter exporter(score);
    const bool previousExportLayout = exporter.configuration()->meiExportLayout();
    const bool previousUseMuseScoreIds = exporter.configuration()->meiUseMuseScoreIds();
    exporter.configuration()->setMeiExportLayout(exportLayout);
    exporter.configuration()->setMeiUseMuseScoreIds(true);
    exporter.setRequirePrecomputedPianomaniaIndices(true);
    exporter.setIncludeExportDate(false);
    const bool ok = exporter.write(out);
    exporter.configuration()->setMeiExportLayout(previousExportLayout);
    exporter.configuration()->setMeiUseMuseScoreIds(previousUseMuseScoreIds);
    if (!ok) {
        return false;
    }

    if (noteRecords) {
        noteRecords->clear();
        noteRecords->reserve(exporter.pianomaniaNoteRecords().size());
        for (const MeiExporter::PianomaniaNoteRecord& record : exporter.pianomaniaNoteRecords()) {
            noteRecords->push_back({ record.note, record.scoreEventId, record.chordId,
                                     record.measureId, record.beat, record.idx });
        }
    }
    return true;
}
}
