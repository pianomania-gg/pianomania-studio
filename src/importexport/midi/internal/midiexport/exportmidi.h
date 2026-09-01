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

#ifndef EXPORTMIDI_H
#define EXPORTMIDI_H

#include <vector>

#include <QFile>

#include "../midishared/midifile.h"
#include "engraving/compat/midi/pausemap.h"
#include "engraving/compat/midi/compatmidirenderinternal.h"

namespace mu::engraving {
class Score;
class TempoMap;
class SynthesizerState;
}

namespace mu::iex::midi {
struct ExportedMidiEventLocator {
    int track = 0;
    int eventOrdinal = 0;
    int absoluteTick = 0;
    int status = 0;
    int channel = 0;
    std::vector<int> data;
};

struct ExportedMidiAudibleEvent {
    int pitch = 0;
    int channel = 0;
    int startTick = 0;
    int endTick = 0;
    int noteOnVelocity = 0;
    int noteOffVelocity = 0;
    ExportedMidiEventLocator noteOn;
    ExportedMidiEventLocator noteOff;
    std::vector<MidiNoteOrigin> origins;
};

struct ExportedMidiProvenance {
    int division = 0;
    std::vector<ExportedMidiAudibleEvent> audibleEvents;
};

//---------------------------------------------------------
//   ExportMidi
//---------------------------------------------------------

class ExportMidi
{
public:
    ExportMidi(engraving::Score* s) { m_score = s; }
    bool write(const QString& name, bool midiExpandRepeats, bool exportRPNs);
    bool write(QIODevice* device, bool midiExpandRepeats, bool exportRPNs);
    bool write(const QString& name, bool midiExpandRepeats, bool exportRPNs, const engraving::SynthesizerState& synthState);
    bool write(QIODevice* device, bool midiExpandRepeats, bool exportRPNs, const engraving::SynthesizerState& synthState);
    bool write(const QString& name, bool midiExpandRepeats, bool exportRPNs, const engraving::SynthesizerState& synthState,
               ExportedMidiProvenance* provenance);
    bool write(QIODevice* device, bool midiExpandRepeats, bool exportRPNs, const engraving::SynthesizerState& synthState,
               ExportedMidiProvenance* provenance);

private:
    void writeHeader(const engraving::CompatMidiRendererInternal::Context& context);
    bool collectPianomaniaProvenance(ExportedMidiProvenance* provenance) const;

    QFile m_file;
    MidiFile m_midiFile;
    engraving::Score* m_score = nullptr;
};
}
#endif // EXPORTMIDI_H
