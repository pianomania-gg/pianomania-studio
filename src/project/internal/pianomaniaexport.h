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
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "global/io/path.h"
#include "types/retval.h"

namespace mu::engraving {
class MasterScore;
class Score;
}

namespace mu::project::pianomania {
struct RepeatExportInfo {
    bool hasRepeats = false;
    bool hasMultipleEndings = false;
    int finalEndingNumber = 0;
};

struct MidiExportedFile {
    enum class Kind {
        Base,
        Repeats
    };

    Kind kind = Kind::Base;
    muse::io::path_t path;
    bool expandRepeats = false;
    std::string traversalId;

    struct Locator {
        int track = 0;
        int eventOrdinal = 0;
        int absoluteTick = 0;
        int status = 0;
        int channel = 0;
        std::vector<int> data;
    };

    struct Origin {
        std::string scoreEventId;
        bool nonVisualOrnament = false;
        int scoreTick = 0;
        int sourceStartTick = 0;
        int sourceEndTick = 0;
        size_t sourceSequence = 0;
    };

    struct AudibleEvent {
        int heardMidiKey = 0;
        int channel = 0;
        int startTick = 0;
        int endTick = 0;
        int noteOnVelocity = 0;
        int noteOffVelocity = 0;
        Locator noteOn;
        Locator noteOff;
        std::vector<Origin> origins;
    };

    struct TraversalSegment {
        std::string repeatSegmentId;
        int sourceStartTick = 0;
        int sourceEndTick = 0;
        int unfoldedStartTick = 0;
        int traversalOrdinal = 0;
        std::string endingId;
    };

    struct VariantCanonicalEvent {
        std::string scoreEventId;
        int scoreTick = 0;
    };

    std::vector<AudibleEvent> audibleEvents;
    std::vector<TraversalSegment> traversalSegments;
    std::vector<VariantCanonicalEvent> variantCanonicalEvents;
};

struct CanonicalEvent {
    std::string scoreEventId;
    std::string chordId;
    std::string eventType;
    int heardMidiKey = 0;
    int idx = 0;
    int scoreTick = 0;
    std::string measureId;
    std::string measureNumber;
    double beat = 0.0;
    int staff = 0;
    int voice = 0;
    std::string simultaneousGroupId;
    std::string aliasOf;
    std::string aliasReason;
};

struct PianomaniaExportResult {
    std::string source;
    muse::io::path_t basePath;
    muse::io::path_t meiPath;
    RepeatExportInfo repeatInfo;
    std::vector<CanonicalEvent> canonicalEvents;
    std::vector<MidiExportedFile> files;
    std::string manifestJson;
};

RepeatExportInfo analyzeRepeatExportInfo(mu::engraving::Score* score);
std::unique_ptr<mu::engraving::MasterScore> buildNoRepeatScoreForFinalEnding(mu::engraving::Score* score, int finalEndingNumber);
bool writeMidiFile(mu::engraving::Score* score, const muse::io::path_t& path, bool expandRepeats, bool exportRpns);
muse::RetVal<PianomaniaExportResult> exportPianomaniaBundle(mu::engraving::Score* score,
                                                            const muse::io::path_t& sourcePath,
                                                            const muse::io::path_t& basePath,
                                                            const muse::io::path_t& meiPath,
                                                            bool exportRpns);
bool writePianomaniaManifest(const muse::io::path_t& path, const PianomaniaExportResult& result);
}
