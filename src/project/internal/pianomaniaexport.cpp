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
#include "pianomaniaexport.h"
#include "pianomaniacompatibility.generated.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/repeatlist.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/volta.h"
#include "importexport/mei/pmmeiexport.h"
#include "importexport/midi/internal/midiexport/exportmidi.h"
#include "importexport/midi/internal/midishared/midifile.h"
#include "types/ret.h"

#include "log.h"

using namespace mu::project::pianomania;

namespace {
struct MeasureRange {
    int startIndex = 0;
    int endIndex = 0;
};

constexpr int MIN_GAMEPLAY_STAFF = 1;
constexpr int MAX_GAMEPLAY_STAFF = 2;

bool isVisibleScoreNote(const mu::engraving::Note* note)
{
    const mu::engraving::Staff* staff = note ? note->staff() : nullptr;
    const mu::engraving::Part* part = staff ? staff->part() : nullptr;
    return note && note->visible() && staff && staff->show() && part && part->show();
}

std::string validateVisibleScoreTopology(const mu::engraving::Score* score)
{
    if (!score) {
        return "Coordinated Pianomania export requires a score";
    }

    int visiblePartCount = 0;
    int visibleStaffCount = 0;
    std::vector<int> visibleStaffIdentities;
    for (const mu::engraving::Part* part : score->parts()) {
        if (!part || !part->show()) {
            continue;
        }

        int partVisibleStaffCount = 0;
        for (const mu::engraving::Staff* staff : part->staves()) {
            if (!staff || !staff->show()) {
                continue;
            }
            ++partVisibleStaffCount;
            ++visibleStaffCount;
            visibleStaffIdentities.push_back(static_cast<int>(staff->idx()) + 1);
        }
        if (partVisibleStaffCount > 0) {
            ++visiblePartCount;
        }
    }

    if (visiblePartCount != 1) {
        return "Coordinated Pianomania export requires exactly one visible score part; found "
               + std::to_string(visiblePartCount);
    }
    if (visibleStaffCount < MIN_GAMEPLAY_STAFF || visibleStaffCount > MAX_GAMEPLAY_STAFF) {
        return "Coordinated Pianomania export requires one or two visible score staves; found "
               + std::to_string(visibleStaffCount);
    }
    for (int staffIdentity : visibleStaffIdentities) {
        if (staffIdentity < MIN_GAMEPLAY_STAFF || staffIdentity > MAX_GAMEPLAY_STAFF) {
            return "Coordinated Pianomania export visible staff identity is outside the supported set {1,2}: staff="
                   + std::to_string(staffIdentity);
        }
    }
    return {};
}

// The coordinated MEI exposes visible written notes only. During its paired
// MIDI render, visible notation is therefore authoritative: legacy invisible
// playback-realization voices are muted and explicitly disabled visible notes
// are rendered by current MuseScore ornament/arpeggio/grace behavior. Restore
// the authored flags before returning so export never mutates the source score.
class ScopedCanonicalNotePlayback
{
public:
    explicit ScopedCanonicalNotePlayback(mu::engraving::Score* score)
    {
        if (!score) {
            return;
        }
        // Score::scanElements skips staves that are not shown, so hidden parts
        // would keep their authored playback and leak into the MIDI timeline.
        // Canonical playback authority covers every written note, visible or not.
        std::vector<mu::engraving::Note*> notes;
        auto collectChord = [&notes](const mu::engraving::Chord* chord) {
            if (!chord) {
                return;
            }
            for (mu::engraving::Note* note : chord->notes()) {
                notes.push_back(note);
            }
        };
        for (const mu::engraving::Segment* segment = score->firstSegment(mu::engraving::SegmentType::ChordRest);
             segment; segment = segment->next1(mu::engraving::SegmentType::ChordRest)) {
            for (mu::engraving::EngravingItem* item : segment->elist()) {
                if (!item || !item->isChord()) {
                    continue;
                }
                const mu::engraving::Chord* chord = mu::engraving::toChord(item);
                for (const mu::engraving::Chord* grace : chord->graceNotes()) {
                    collectChord(grace);
                }
                collectChord(chord);
            }
        }
        for (mu::engraving::Note* note : notes) {
            const bool shouldPlay = isVisibleScoreNote(note);
            if (note->play() == shouldPlay) {
                continue;
            }
            m_changedNotes.push_back({ note, note->play() });
            note->setPlay(shouldPlay);
        }
    }

    ~ScopedCanonicalNotePlayback()
    {
        for (const auto& [note, play] : m_changedNotes) {
            note->setPlay(play);
        }
    }

private:
    std::vector<std::pair<mu::engraving::Note*, bool> > m_changedNotes;
};

void mergeMeasureRanges(std::vector<MeasureRange>& ranges)
{
    if (ranges.empty()) {
        return;
    }

    std::sort(ranges.begin(), ranges.end(), [](const MeasureRange& a, const MeasureRange& b) {
        return a.startIndex < b.startIndex;
    });

    std::vector<MeasureRange> merged;
    merged.reserve(ranges.size());

    for (const auto& range : ranges) {
        if (merged.empty() || range.startIndex > merged.back().endIndex + 1) {
            merged.push_back(range);
        } else {
            merged.back().endIndex = std::max(merged.back().endIndex, range.endIndex);
        }
    }

    ranges.swap(merged);
}

void removeNonFinalEndings(mu::engraving::MasterScore* score, int finalEndingNumber)
{
    if (!score || finalEndingNumber <= 0) {
        return;
    }

    std::vector<mu::engraving::Measure*> measures;
    for (auto* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        measures.push_back(measure);
    }

    std::unordered_map<mu::engraving::Measure*, int> measureIndex;
    measureIndex.reserve(measures.size());
    for (size_t i = 0; i < measures.size(); ++i) {
        measureIndex.emplace(measures[i], static_cast<int>(i));
    }

    std::vector<MeasureRange> rangesToRemove;
    std::vector<mu::engraving::Volta*> voltasToRemove;

    for (const auto& entry : score->spannerMap().map()) {
        auto* spanner = entry.second;
        if (!spanner || !spanner->isVolta() || !spanner->playSpanner()) {
            continue;
        }

        auto* volta = mu::engraving::toVolta(spanner);
        if (!volta->hasEnding(finalEndingNumber)) {
            mu::engraving::Measure* startMeasure = volta->startMeasure();
            mu::engraving::Measure* endMeasure = volta->endMeasure();
            if (!startMeasure) {
                startMeasure = volta->findStartMeasure();
            }
            if (!endMeasure) {
                endMeasure = volta->findEndMeasure();
            }

            auto startIt = measureIndex.find(startMeasure);
            auto endIt = measureIndex.find(endMeasure);
            if (startIt != measureIndex.end() && endIt != measureIndex.end()) {
                int startIndex = startIt->second;
                int endIndex = endIt->second;
                if (startIndex > endIndex) {
                    std::swap(startIndex, endIndex);
                }
                rangesToRemove.push_back({ startIndex, endIndex });
            }

            voltasToRemove.push_back(volta);
        }
    }

    for (auto* volta : voltasToRemove) {
        score->removeSpanner(volta);
    }

    if (rangesToRemove.empty()) {
        return;
    }

    mergeMeasureRanges(rangesToRemove);

    for (auto it = rangesToRemove.rbegin(); it != rangesToRemove.rend(); ++it) {
        int startIndex = it->startIndex;
        int endIndex = it->endIndex;
        if (startIndex < 0 || endIndex < 0
            || startIndex >= static_cast<int>(measures.size())
            || endIndex >= static_cast<int>(measures.size())) {
            continue;
        }

        score->deleteMeasures(measures[startIndex], measures[endIndex]);
    }
}

std::string midiKindName(MidiExportedFile::Kind kind)
{
    switch (kind) {
    case MidiExportedFile::Kind::Base:
        return "base";
    case MidiExportedFile::Kind::Repeats:
        return "repeats";
    }
    return {};
}

std::string manifestFileName(const muse::io::path_t& path)
{
    return QFileInfo(path.toQString()).fileName().toStdString();
}

std::string stableXmlId(const mu::engraving::EngravingItem* item)
{
    if (!item) {
        return {};
    }
    const mu::engraving::EID eid = item->eid();
    if (!eid.isValid()) {
        return {};
    }
    muse::String encoded = muse::String::fromStdString(eid.toStdString());
    return "mscore-" + encoded.replace('/', '.').replace('+', '-').toStdString();
}

bool isIdentityTokenCharacter(char value)
{
    return std::isalnum(static_cast<unsigned char>(value)) || value == '.' || value == '_' || value == '-';
}

bool isMuseScoreIdentity(const std::string& value)
{
    static const std::regex pattern(R"(^mscore-[A-Za-z0-9._-]+$)");
    return std::regex_match(value, pattern);
}

std::string publicIdentity(size_t ordinal)
{
    std::ostringstream stream;
    stream << "pm-score-" << std::setw(6) << std::setfill('0') << ordinal;
    return stream.str();
}

std::string replaceIdentityTokens(const std::string& value,
                                  const std::map<std::string, std::string>& replacements)
{
    std::string result;
    result.reserve(value.size());
    size_t cursor = 0;
    while (cursor < value.size()) {
        const size_t start = value.find("mscore-", cursor);
        if (start == std::string::npos) {
            result.append(value, cursor, std::string::npos);
            break;
        }
        result.append(value, cursor, start - cursor);
        size_t end = start + 7;
        while (end < value.size() && isIdentityTokenCharacter(value[end])) {
            ++end;
        }
        const std::string token = value.substr(start, end - start);
        const auto replacement = replacements.find(token);
        result += replacement == replacements.end() ? token : replacement->second;
        cursor = end;
    }
    return result;
}

bool containsMuseScoreIdentity(const std::string& value)
{
    size_t cursor = 0;
    while ((cursor = value.find("mscore-", cursor)) != std::string::npos) {
        size_t end = cursor + 7;
        while (end < value.size() && isIdentityTokenCharacter(value[end])) {
            ++end;
        }
        if (isMuseScoreIdentity(value.substr(cursor, end - cursor))) {
            return true;
        }
        cursor = end;
    }
    return false;
}

bool containsMuseScoreMeiIdentity(const std::string& value)
{
    static const std::regex declarationPattern(R"(xml:id="mscore-[A-Za-z0-9._-]+")");
    static const std::regex referencePattern(R"(#mscore-[A-Za-z0-9._-]+)");
    static const std::regex pianomaniaReferencePattern(
        R"((?:pm:covered-id|pm:covered-ids|pm:coveredUuids|pm:terminal-ids|pm:post-terminal-ids)="[^"]*mscore-[^"]*")");
    return std::regex_search(value, declarationPattern)
           || std::regex_search(value, referencePattern)
           || std::regex_search(value, pianomaniaReferencePattern);
}

bool canonicalizePublicIdentityGraph(std::string& meiData, PianomaniaExportResult& result)
{
    static const std::regex declarationPattern(R"REGEX(xml:id="(mscore-[A-Za-z0-9._-]+)")REGEX");
    static const std::regex publicDeclarationPattern(R"REGEX(xml:id="(pm-score-[0-9]{6})")REGEX");
    static const std::regex referencePattern(R"(#(mscore-[A-Za-z0-9._-]+))");
    static const std::regex pianomaniaReferencePattern(
        R"REGEX((?:pm:covered-id|pm:covered-ids|pm:coveredUuids|pm:terminal-ids|pm:post-terminal-ids)="([^"]*)")REGEX");

    std::map<std::string, std::string> replacements;
    std::set<std::string> generated;
    size_t ordinal = 0;
    for (std::sregex_iterator it(meiData.begin(), meiData.end(), declarationPattern), end; it != end; ++it) {
        const std::string identity = (*it)[1].str();
        const std::string replacement = publicIdentity(++ordinal);
        if (!replacements.emplace(identity, replacement).second || !generated.insert(replacement).second) {
            LOGE() << "Coordinated Pianomania MEI contains duplicate score identities";
            return false;
        }
    }
    if (replacements.empty()) {
        LOGE() << "Coordinated Pianomania MEI contains no MuseScore identity declarations";
        return false;
    }
    for (std::sregex_iterator it(meiData.begin(), meiData.end(), publicDeclarationPattern), end; it != end; ++it) {
        if (generated.count((*it)[1].str())) {
            LOGE() << "Coordinated Pianomania public score identity collides";
            return false;
        }
    }
    for (std::sregex_iterator it(meiData.begin(), meiData.end(), referencePattern), end; it != end; ++it) {
        if (!replacements.count((*it)[1].str())) {
            LOGE() << "Coordinated Pianomania MEI contains an undeclared MuseScore reference";
            return false;
        }
    }
    for (std::sregex_iterator it(meiData.begin(), meiData.end(), pianomaniaReferencePattern), end; it != end; ++it) {
        std::istringstream tokens((*it)[1].str());
        std::string identity;
        while (tokens >> identity) {
            if (isMuseScoreIdentity(identity) && !replacements.count(identity)) {
                LOGE() << "Coordinated Pianomania MEI contains an undeclared spanner identity";
                return false;
            }
        }
    }

    // Single-note chords have no public MEI chord element. Keep their
    // manifest identities after the identities declared by the canonical MEI.
    for (const CanonicalEvent& event : result.canonicalEvents) {
        if (isMuseScoreIdentity(event.chordId) && !replacements.count(event.chordId)) {
            replacements.emplace(event.chordId, publicIdentity(++ordinal));
        }
    }

    meiData = replaceIdentityTokens(meiData, replacements);
    if (containsMuseScoreMeiIdentity(meiData)) {
        LOGE() << "Coordinated Pianomania MEI retains an internal MuseScore identity";
        return false;
    }

    const auto rewrite = [&replacements](std::string& value) {
        value = replaceIdentityTokens(value, replacements);
        return !containsMuseScoreIdentity(value);
    };
    for (CanonicalEvent& event : result.canonicalEvents) {
        if (!rewrite(event.scoreEventId) || !rewrite(event.chordId) || !rewrite(event.measureId)
            || !rewrite(event.simultaneousGroupId) || !rewrite(event.aliasOf)) {
            LOGE() << "Coordinated Pianomania canonical event retains an internal identity";
            return false;
        }
    }
    for (MidiExportedFile& file : result.files) {
        for (MidiExportedFile::TraversalSegment& segment : file.traversalSegments) {
            if (!rewrite(segment.repeatSegmentId) || !rewrite(segment.endingId)) {
                LOGE() << "Coordinated Pianomania traversal retains an internal identity";
                return false;
            }
        }
        for (MidiExportedFile::VariantCanonicalEvent& event : file.variantCanonicalEvents) {
            if (!rewrite(event.scoreEventId)) {
                LOGE() << "Coordinated Pianomania variant retains an internal identity";
                return false;
            }
        }
        for (MidiExportedFile::AudibleEvent& event : file.audibleEvents) {
            for (MidiExportedFile::Origin& origin : event.origins) {
                if (!rewrite(origin.scoreEventId)) {
                    LOGE() << "Coordinated Pianomania MIDI provenance retains an internal identity";
                    return false;
                }
            }
        }
    }
    return true;
}

std::string endingIdForSegment(mu::engraving::Score* score, const mu::engraving::RepeatSegment* segment, bool expandRepeats)
{
    if (!score || !segment || !expandRepeats) {
        return {};
    }
    for (const auto& entry : score->spannerMap().map()) {
        const mu::engraving::Spanner* spanner = entry.second;
        if (!spanner || !spanner->isVolta() || !spanner->playSpanner()) {
            continue;
        }
        const mu::engraving::Volta* volta = mu::engraving::toVolta(spanner);
        const mu::engraving::Measure* start = volta->startMeasure();
        const mu::engraving::Measure* end = volta->endMeasure();
        if (!start || !end || !volta->hasEnding(segment->playbackCount)) {
            continue;
        }
        if (segment->tick < start->tick().ticks() || segment->tick > end->endTick().ticks()) {
            continue;
        }
        std::string id = stableXmlId(volta);
        if (!id.empty()) {
            return id;
        }
        return "ending:" + std::to_string(start->tick().ticks()) + "-" + std::to_string(end->endTick().ticks());
    }
    return {};
}

std::vector<MidiExportedFile::TraversalSegment> traversalSegments(mu::engraving::Score* score,
                                                                  const std::string& traversalId,
                                                                  bool expandRepeats)
{
    std::vector<MidiExportedFile::TraversalSegment> result;
    if (!score) {
        return result;
    }
    const mu::engraving::RepeatList& repeatList = score->repeatList(expandRepeats, false);
    result.reserve(repeatList.size());
    int ordinal = 0;
    for (const mu::engraving::RepeatSegment* segment : repeatList) {
        if (!segment || segment->isEmpty()) {
            continue;
        }
        ++ordinal;
        const int sourceEndTick = segment->tick + segment->len();
        const std::string endingId = endingIdForSegment(score, segment, expandRepeats);
        std::string segmentId = "segment:" + std::to_string(segment->tick) + "-" + std::to_string(sourceEndTick)
                                + ":pass:" + std::to_string(segment->playbackCount);
        if (!endingId.empty()) {
            segmentId += ":" + endingId;
        }
        result.push_back({ segmentId, segment->tick, sourceEndTick, segment->utick, ordinal, endingId });
    }
    if (result.empty()) {
        LOGE() << "Pianomania traversal contains no segments: " << traversalId;
    }
    return result;
}

MidiExportedFile::Locator stableLocator(const mu::iex::midi::ExportedMidiEventLocator& locator)
{
    return { locator.track, locator.eventOrdinal, locator.absoluteTick, locator.status, locator.channel, locator.data };
}

bool stabilizeProvenance(const mu::iex::midi::ExportedMidiProvenance& source, MidiExportedFile& destination)
{
    destination.audibleEvents.clear();
    destination.audibleEvents.reserve(source.audibleEvents.size());
    for (const mu::iex::midi::ExportedMidiAudibleEvent& event : source.audibleEvents) {
        MidiExportedFile::AudibleEvent stable;
        stable.heardMidiKey = event.pitch;
        stable.channel = event.channel;
        stable.startTick = event.startTick;
        stable.endTick = event.endTick;
        stable.noteOnVelocity = event.noteOnVelocity;
        stable.noteOffVelocity = event.noteOffVelocity;
        stable.noteOn = stableLocator(event.noteOn);
        stable.noteOff = stableLocator(event.noteOff);
        stable.origins.reserve(event.origins.size());
        for (const mu::iex::midi::MidiNoteOrigin& origin : event.origins) {
            const std::string scoreEventId = stableXmlId(origin.note);
            const mu::engraving::Chord* chord = origin.note ? origin.note->chord() : nullptr;
            if (scoreEventId.empty() || !chord) {
                LOGE() << "Pianomania MIDI origin has no stable score identity";
                return false;
            }
            stable.origins.push_back({ scoreEventId, origin.nonVisualOrnament, chord->tick().ticks(),
                                       origin.sourceStartTick, origin.sourceEndTick, origin.sourceSequence });
        }
        destination.audibleEvents.push_back(std::move(stable));
    }
    return true;
}

bool locatorMatches(const mu::iex::midi::MidiFile& midi, const MidiExportedFile::Locator& locator)
{
    if (locator.track < 0 || locator.track >= static_cast<int>(midi.tracks().size())) {
        return false;
    }
    const auto& events = midi.tracks()[locator.track].events();
    if (locator.eventOrdinal < 0 || locator.eventOrdinal >= static_cast<int>(events.size())) {
        return false;
    }
    auto it = events.cbegin();
    std::advance(it, locator.eventOrdinal);
    const mu::iex::midi::MidiEvent& event = it->second;
    const int status = event.type() | event.channel();
    return it->first == locator.absoluteTick
           && status == locator.status
           && event.channel() == locator.channel
           && locator.data.size() == 2
           && event.dataA() == locator.data[0]
           && event.dataB() == locator.data[1];
}

bool verifyWrittenMidi(const MidiExportedFile& file)
{
    QFile input(file.path.toQString());
    if (!input.open(QIODevice::ReadOnly)) {
        LOGE() << "Cannot reopen Pianomania MIDI for locator verification: " << file.path.toQString();
        return false;
    }
    mu::iex::midi::MidiFile midi;
    if (!midi.read(&input)) {
        LOGE() << "Cannot parse Pianomania MIDI for locator verification: " << file.path.toQString();
        return false;
    }
    for (const MidiExportedFile::AudibleEvent& event : file.audibleEvents) {
        if (!locatorMatches(midi, event.noteOn) || !locatorMatches(midi, event.noteOff)) {
            LOGE() << "Pianomania MIDI locator does not reproduce exported file: " << file.path.toQString();
            return false;
        }
    }
    return true;
}

bool buildCanonicalEvents(const std::vector<mu::iex::mei::PmMeiNoteRecord>& records,
                          std::vector<CanonicalEvent>& destination)
{
    destination.clear();
    destination.reserve(records.size());
    std::map<std::pair<int, int>, size_t> primaryByLocator;
    std::set<std::string> identities;

    for (const mu::iex::mei::PmMeiNoteRecord& record : records) {
        const mu::engraving::Note* note = record.note;
        const mu::engraving::Chord* chord = note ? note->chord() : nullptr;
        const mu::engraving::Measure* measure = chord ? chord->measure() : nullptr;
        if (!note || !chord || !measure || record.scoreEventId.empty()
            || record.chordId.empty() || record.measureId.empty()) {
            LOGE() << "Coordinated Pianomania MEI record is missing score identity";
            return false;
        }
        if (stableXmlId(note) != record.scoreEventId || stableXmlId(chord) != record.chordId
            || stableXmlId(measure) != record.measureId) {
            LOGE() << "Coordinated Pianomania MEI identities do not match score EIDs";
            return false;
        }
        if (!identities.insert(record.scoreEventId).second) {
            LOGE() << "Duplicate coordinated Pianomania score event identity: " << record.scoreEventId;
            return false;
        }

        bool beatOk = false;
        const double beat = QString::fromStdString(record.beat).toDouble(&beatOk);
        if (!beatOk) {
            LOGE() << "Coordinated Pianomania MEI beat is not numeric: " << record.beat;
            return false;
        }

        CanonicalEvent event;
        event.scoreEventId = record.scoreEventId;
        event.chordId = record.chordId;
        event.eventType = chord->isGrace() ? "written_grace" : "written";
        event.heardMidiKey = note->ppitch();
        event.idx = record.idx;
        event.scoreTick = chord->tick().ticks();
        event.measureId = record.measureId;
        event.measureNumber = std::to_string(measure->no() + 1);
        event.beat = beat;
        event.staff = static_cast<int>(note->staffIdx()) + 1;
        if (event.staff < MIN_GAMEPLAY_STAFF || event.staff > MAX_GAMEPLAY_STAFF) {
            LOGE() << "Coordinated Pianomania canonical event staff is outside the supported set {1,2}: "
                   << event.scoreEventId << ", staff=" << event.staff;
            return false;
        }
        event.voice = static_cast<int>(note->voice()) + 1;
        event.simultaneousGroupId = chord->notes().size() > 1
                                        ? record.chordId
                                        : record.measureId + "@" + record.beat;

        const std::pair<int, int> locator(event.heardMidiKey, event.idx);
        auto primary = primaryByLocator.find(locator);
        if (primary == primaryByLocator.end()) {
            primaryByLocator.emplace(locator, destination.size());
        } else {
            CanonicalEvent& target = destination[primary->second];
            const mu::engraving::Note* targetNote = records[primary->second].note;
            event.aliasOf = target.scoreEventId;
            if (event.scoreTick == target.scoreTick) {
                event.aliasReason = "same_onset";
            } else if (note->tieBack()) {
                event.aliasReason = "tie_continuation";
            } else if (targetNote && targetNote->tieBack()
                       && targetNote->tieBack()->startNote() == note) {
                // MEI records are emitted in staff/layer order, so a
                // cross-voice tie continuation can be encountered before its
                // earlier tie start. Make the written attack authoritative
                // and retroactively classify the continuation as its alias.
                target.aliasOf = event.scoreEventId;
                target.aliasReason = "tie_continuation";
                event.aliasOf.clear();
                primary->second = destination.size();
            } else {
                LOGE() << "Canonical locator is reused without same-onset or tie alias: "
                       << event.scoreEventId;
                return false;
            }
        }
        destination.push_back(std::move(event));
    }
    return !destination.empty();
}

std::vector<MidiExportedFile::VariantCanonicalEvent> variantCanonicalEvents(
    const std::vector<mu::iex::mei::PmMeiNoteRecord>& records)
{
    std::vector<MidiExportedFile::VariantCanonicalEvent> result;
    result.reserve(records.size());
    for (const mu::iex::mei::PmMeiNoteRecord& record : records) {
        const mu::engraving::Chord* chord = record.note ? record.note->chord() : nullptr;
        if (!chord || record.scoreEventId.empty()) {
            return {};
        }
        result.push_back({ record.scoreEventId, chord->tick().ticks() });
    }
    return result;
}

QJsonObject locatorJson(const MidiExportedFile::Locator& locator)
{
    QJsonArray data;
    for (int value : locator.data) {
        data.append(value);
    }
    QJsonObject object;
    object["track"] = locator.track;
    object["event_ordinal"] = locator.eventOrdinal;
    object["absolute_tick"] = locator.absoluteTick;
    object["status"] = locator.status;
    object["channel"] = locator.channel;
    object["data"] = data;
    return object;
}

QJsonValue nullableString(const std::string& value)
{
    return value.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QString::fromStdString(value));
}

struct BuiltOccurrence {
    std::string occurrenceId;
    std::string identity;
    int traversalOrdinal = 0;
    std::string repeatSegmentId;
    std::string endingId;
    std::string audibleEventId;
};

struct BuiltAudibleEvent {
    const MidiExportedFile::AudibleEvent* source = nullptr;
    std::string audibleEventId;
    std::string eventType;
    int idx = -1;
    std::vector<std::string> occurrenceIds;
    std::set<std::string> aliasIds;
    std::string ornamentDefinitionId;
};

struct BuiltOrnamentDefinition {
    std::string ornamentDefinitionId;
    std::string targetScoreEventId;
    int heardMidiKey = 0;
    int playbackOrdinal = 0;
};

struct BuiltFile {
    const MidiExportedFile* source = nullptr;
    std::vector<BuiltAudibleEvent> audibleEvents;
    std::vector<BuiltOccurrence> visualOccurrences;
    std::vector<BuiltOccurrence> ornamentOccurrences;
};

std::vector<const MidiExportedFile::TraversalSegment*> segmentsForTick(const MidiExportedFile& file, int scoreTick)
{
    std::vector<const MidiExportedFile::TraversalSegment*> result;
    for (const MidiExportedFile::TraversalSegment& segment : file.traversalSegments) {
        if (scoreTick >= segment.sourceStartTick && scoreTick < segment.sourceEndTick) {
            result.push_back(&segment);
        }
    }
    return result;
}

bool buildManifestModel(const PianomaniaExportResult& result,
                        std::vector<BuiltOrnamentDefinition>& ornamentDefinitions,
                        std::vector<BuiltFile>& builtFiles)
{
    std::map<std::string, const CanonicalEvent*> canonical;
    for (const CanonicalEvent& event : result.canonicalEvents) {
        canonical.emplace(event.scoreEventId, &event);
    }

    const MidiExportedFile* baseFile = nullptr;
    for (const MidiExportedFile& file : result.files) {
        if (file.kind == MidiExportedFile::Kind::Base) {
            baseFile = &file;
            break;
        }
    }
    if (!baseFile) {
        LOGE() << "Pianomania manifest v3 requires a base MIDI file";
        return false;
    }

    std::map<std::string, std::vector<const MidiExportedFile::AudibleEvent*> > baseOrnamentsByTarget;
    for (const MidiExportedFile::AudibleEvent& event : baseFile->audibleEvents) {
        if (event.noteOffVelocity != 127) {
            continue;
        }
        if (event.origins.size() != 1 || !event.origins.front().nonVisualOrnament) {
            std::string origins;
            for (const MidiExportedFile::Origin& origin : event.origins) {
                if (!origins.empty()) {
                    origins += ",";
                }
                origins += origin.scoreEventId + "@" + std::to_string(origin.scoreTick)
                           + (origin.nonVisualOrnament ? ":ornament" : ":visual");
            }
            LOGE() << "Each score-declared ornament attack must have one stable target origin: key="
                   << event.heardMidiKey << ", start=" << event.startTick
                   << ", origins=[" << origins << "]";
            return false;
        }
        baseOrnamentsByTarget[event.origins.front().scoreEventId].push_back(&event);
    }
    std::map<std::string, std::vector<const BuiltOrnamentDefinition*> > definitionsByTarget;
    for (const auto& [target, events] : baseOrnamentsByTarget) {
        if (!canonical.count(target)) {
            LOGE() << "Ornament target is not a canonical score event: " << target;
            return false;
        }
        int ordinal = 0;
        for (const MidiExportedFile::AudibleEvent* event : events) {
            BuiltOrnamentDefinition definition;
            definition.targetScoreEventId = target;
            definition.heardMidiKey = event->heardMidiKey;
            definition.playbackOrdinal = ordinal;
            definition.ornamentDefinitionId = "ornament:" + target + ":" + std::to_string(ordinal++);
            ornamentDefinitions.push_back(std::move(definition));
        }
    }
    for (const BuiltOrnamentDefinition& definition : ornamentDefinitions) {
        definitionsByTarget[definition.targetScoreEventId].push_back(&definition);
    }

    for (const MidiExportedFile& file : result.files) {
        BuiltFile built;
        built.source = &file;
        const std::string kind = midiKindName(file.kind);
        built.audibleEvents.reserve(file.audibleEvents.size());
        for (size_t i = 0; i < file.audibleEvents.size(); ++i) {
            const MidiExportedFile::AudibleEvent& event = file.audibleEvents[i];
            BuiltAudibleEvent audible;
            audible.source = &event;
            audible.audibleEventId = kind + ":audible:" + std::to_string(i);
            const bool ornament = event.noteOffVelocity == 127;
            if (event.origins.empty()) {
                LOGE() << "Every exported audible event must have score provenance";
                return false;
            }
            for (const MidiExportedFile::Origin& origin : event.origins) {
                if (origin.nonVisualOrnament != ornament) {
                    LOGE() << "Audible event origin classification disagrees with reserved note-off velocity";
                    return false;
                }
            }
            if (ornament) {
                audible.eventType = "nonvisual_ornament";
            }
            built.audibleEvents.push_back(std::move(audible));
        }

        std::map<std::string, int> variantTicks;
        for (const MidiExportedFile::VariantCanonicalEvent& event : file.variantCanonicalEvents) {
            if (!variantTicks.emplace(event.scoreEventId, event.scoreTick).second) {
                LOGE() << "Variant repeats a canonical event definition: " << event.scoreEventId;
                return false;
            }
        }

        std::map<std::string, std::vector<size_t> > visualAudibleByScoreId;
        std::map<std::string, std::vector<size_t> > ornamentAudibleByTarget;
        for (size_t audibleIndex = 0; audibleIndex < file.audibleEvents.size(); ++audibleIndex) {
            const MidiExportedFile::AudibleEvent& event = file.audibleEvents[audibleIndex];
            std::set<std::string> seenOrigins;
            for (const MidiExportedFile::Origin& origin : event.origins) {
                if (!seenOrigins.insert(origin.scoreEventId).second) {
                    continue;
                }
                if (origin.nonVisualOrnament) {
                    ornamentAudibleByTarget[origin.scoreEventId].push_back(audibleIndex);
                } else {
                    visualAudibleByScoreId[origin.scoreEventId].push_back(audibleIndex);
                }
            }
        }

        std::map<std::string, std::vector<size_t> > occurrenceAudiblesByScoreId;
        std::vector<const CanonicalEvent*> pendingSharedAttackAliases;
        for (const CanonicalEvent& event : result.canonicalEvents) {
            auto tickIt = variantTicks.find(event.scoreEventId);
            if (tickIt == variantTicks.end()) {
                continue;
            }
            const auto segments = segmentsForTick(file, tickIt->second);
            if (segments.empty()) {
                LOGE() << "Variant canonical event has no traversal segment: " << event.scoreEventId;
                return false;
            }
            auto audibleIt = visualAudibleByScoreId.find(event.scoreEventId);
            const std::vector<size_t> direct = audibleIt == visualAudibleByScoreId.end()
                                                       ? std::vector<size_t>() : audibleIt->second;
            if (direct.empty() && !event.aliasOf.empty()) {
                pendingSharedAttackAliases.push_back(&event);
                continue;
            }
            if (direct.size() != segments.size()) {
                LOGE() << "Canonical event playback occurrence count disagrees with traversal: "
                       << event.scoreEventId << ", playback=" << direct.size()
                       << ", traversal=" << segments.size()
                       << ", nonvisual_attacks=" << ornamentAudibleByTarget[event.scoreEventId].size();
                for (size_t audibleIndex : direct) {
                    const MidiExportedFile::AudibleEvent& source = file.audibleEvents[audibleIndex];
                    LOGE() << "Direct attack detail: key=" << source.heardMidiKey
                           << ", start=" << source.startTick << ", end=" << source.endTick;
                }
                return false;
            }
            for (size_t i = 0; i < segments.size(); ++i) {
                const size_t audibleIndex = direct[i];
                BuiltAudibleEvent& audible = built.audibleEvents[audibleIndex];
                const MidiExportedFile::AudibleEvent& source = *audible.source;
                if (source.heardMidiKey != event.heardMidiKey || source.noteOffVelocity == 127) {
                    LOGE() << "Visual playback event disagrees with canonical score identity";
                    return false;
                }
                if (audible.eventType.empty()) {
                    audible.eventType = event.eventType;
                    audible.idx = event.idx;
                } else if (audible.eventType != event.eventType || audible.idx != event.idx) {
                    LOGE() << "Same attack merges incompatible canonical visual identities";
                    return false;
                }
                if (!event.aliasOf.empty()) {
                    audible.aliasIds.insert(event.scoreEventId);
                }
                BuiltOccurrence occurrence;
                occurrence.occurrenceId = kind + ":visual:" + event.scoreEventId + ":"
                                          + std::to_string(segments[i]->traversalOrdinal);
                occurrence.identity = event.scoreEventId;
                occurrence.traversalOrdinal = segments[i]->traversalOrdinal;
                occurrence.repeatSegmentId = segments[i]->repeatSegmentId;
                occurrence.endingId = segments[i]->endingId;
                occurrence.audibleEventId = audible.audibleEventId;
                audible.occurrenceIds.push_back(occurrence.occurrenceId);
                occurrenceAudiblesByScoreId[event.scoreEventId].push_back(audibleIndex);
                built.visualOccurrences.push_back(std::move(occurrence));
            }
        }

        for (const CanonicalEvent* event : pendingSharedAttackAliases) {
            auto tickIt = variantTicks.find(event->scoreEventId);
            const auto segments = tickIt == variantTicks.end() ? std::vector<const MidiExportedFile::TraversalSegment*>()
                                                                : segmentsForTick(file, tickIt->second);
            auto targetIt = occurrenceAudiblesByScoreId.find(event->aliasOf);
            if (segments.empty() || targetIt == occurrenceAudiblesByScoreId.end()
                || targetIt->second.size() != segments.size()) {
                LOGE() << "Canonical alias cannot resolve its shared audible attack: "
                       << event->scoreEventId;
                return false;
            }
            for (size_t i = 0; i < segments.size(); ++i) {
                BuiltAudibleEvent& audible = built.audibleEvents[targetIt->second[i]];
                audible.aliasIds.insert(event->scoreEventId);
                BuiltOccurrence occurrence;
                occurrence.occurrenceId = kind + ":visual:" + event->scoreEventId + ":"
                                          + std::to_string(segments[i]->traversalOrdinal);
                occurrence.identity = event->scoreEventId;
                occurrence.traversalOrdinal = segments[i]->traversalOrdinal;
                occurrence.repeatSegmentId = segments[i]->repeatSegmentId;
                occurrence.endingId = segments[i]->endingId;
                occurrence.audibleEventId = audible.audibleEventId;
                audible.occurrenceIds.push_back(occurrence.occurrenceId);
                occurrenceAudiblesByScoreId[event->scoreEventId].push_back(targetIt->second[i]);
                built.visualOccurrences.push_back(std::move(occurrence));
            }
        }

        for (const auto& [target, audibleIndices] : ornamentAudibleByTarget) {
            auto definitionsIt = definitionsByTarget.find(target);
            auto tickIt = variantTicks.find(target);
            if (definitionsIt == definitionsByTarget.end() || tickIt == variantTicks.end()) {
                LOGE() << "Ornament playback cannot resolve target/definition: " << target;
                return false;
            }
            const auto segments = segmentsForTick(file, tickIt->second);
            const auto& definitions = definitionsIt->second;
            if (segments.empty() || audibleIndices.size() != segments.size() * definitions.size()) {
                LOGE() << "Ornament occurrence count disagrees with traversal for target " << target;
                return false;
            }
            for (size_t occurrenceIndex = 0; occurrenceIndex < segments.size(); ++occurrenceIndex) {
                for (size_t definitionIndex = 0; definitionIndex < definitions.size(); ++definitionIndex) {
                    const size_t flatIndex = occurrenceIndex * definitions.size() + definitionIndex;
                    BuiltAudibleEvent& audible = built.audibleEvents[audibleIndices[flatIndex]];
                    const BuiltOrnamentDefinition& definition = *definitions[definitionIndex];
                    if (audible.source->heardMidiKey != definition.heardMidiKey
                        || audible.source->noteOffVelocity != 127 || !audible.occurrenceIds.empty()) {
                        LOGE() << "Ornament occurrence does not reproduce its base definition";
                        return false;
                    }
                    audible.ornamentDefinitionId = definition.ornamentDefinitionId;
                    BuiltOccurrence occurrence;
                    occurrence.occurrenceId = kind + ":ornament:" + definition.ornamentDefinitionId + ":"
                                              + std::to_string(segments[occurrenceIndex]->traversalOrdinal);
                    occurrence.identity = definition.ornamentDefinitionId;
                    occurrence.traversalOrdinal = segments[occurrenceIndex]->traversalOrdinal;
                    occurrence.repeatSegmentId = segments[occurrenceIndex]->repeatSegmentId;
                    occurrence.endingId = segments[occurrenceIndex]->endingId;
                    occurrence.audibleEventId = audible.audibleEventId;
                    audible.occurrenceIds.push_back(occurrence.occurrenceId);
                    built.ornamentOccurrences.push_back(std::move(occurrence));
                }
            }
        }

        for (const BuiltAudibleEvent& audible : built.audibleEvents) {
            if (audible.eventType.empty() || audible.occurrenceIds.empty()) {
                LOGE() << "Exported audible event is not fully classified in manifest v3";
                return false;
            }
        }
        builtFiles.push_back(std::move(built));
    }
    return true;
}

QJsonObject occurrenceJson(const BuiltOccurrence& occurrence, bool ornament)
{
    QJsonObject object;
    object["occurrence_id"] = QString::fromStdString(occurrence.occurrenceId);
    object[ornament ? "ornament_definition_id" : "score_event_id"] = QString::fromStdString(occurrence.identity);
    object["traversal_ordinal"] = occurrence.traversalOrdinal;
    object["repeat_segment_id"] = QString::fromStdString(occurrence.repeatSegmentId);
    object["ending_id"] = nullableString(occurrence.endingId);
    object["audible_event_id"] = QString::fromStdString(occurrence.audibleEventId);
    return object;
}

bool buildManifestJson(PianomaniaExportResult& result)
{
    std::vector<BuiltOrnamentDefinition> ornamentDefinitions;
    std::vector<BuiltFile> files;
    if (!buildManifestModel(result, ornamentDefinitions, files)) {
        return false;
    }

    QJsonObject score;
    score["source"] = QString::fromStdString(result.source);
    score["base_path"] = QString::fromStdString(manifestFileName(result.basePath));
    QJsonObject mei;
    mei["path"] = QString::fromStdString(manifestFileName(result.meiPath));
    score["mei"] = mei;

    QJsonObject repeatMetadata;
    repeatMetadata["has_repeats"] = result.repeatInfo.hasRepeats;
    repeatMetadata["has_multiple_endings"] = result.repeatInfo.hasMultipleEndings;
    repeatMetadata["final_ending_number"] = result.repeatInfo.finalEndingNumber;

    std::map<std::string, QJsonArray> memberships;
    for (const MidiExportedFile& file : result.files) {
        for (const MidiExportedFile::VariantCanonicalEvent& event : file.variantCanonicalEvents) {
            memberships[event.scoreEventId].append(QString::fromStdString(midiKindName(file.kind)));
        }
    }

    QJsonArray canonicalEvents;
    for (const CanonicalEvent& event : result.canonicalEvents) {
        QJsonObject object;
        object["score_event_id"] = QString::fromStdString(event.scoreEventId);
        object["chord_id"] = QString::fromStdString(event.chordId);
        object["event_type"] = QString::fromStdString(event.eventType);
        object["heard_midi_key"] = event.heardMidiKey;
        object["idx"] = event.idx;
        object["score_tick"] = event.scoreTick;
        object["measure_id"] = QString::fromStdString(event.measureId);
        object["measure_number"] = QString::fromStdString(event.measureNumber);
        object["beat"] = event.beat;
        object["staff"] = event.staff;
        object["voice"] = event.voice;
        object["simultaneous_group_id"] = QString::fromStdString(event.simultaneousGroupId);
        object["alias_of"] = nullableString(event.aliasOf);
        object["alias_reason"] = nullableString(event.aliasReason);
        object["variant_membership"] = memberships[event.scoreEventId];
        canonicalEvents.append(object);
    }

    QJsonArray ornamentJson;
    for (const BuiltOrnamentDefinition& definition : ornamentDefinitions) {
        QJsonObject object;
        object["ornament_definition_id"] = QString::fromStdString(definition.ornamentDefinitionId);
        object["target_score_event_id"] = QString::fromStdString(definition.targetScoreEventId);
        object["heard_midi_key"] = definition.heardMidiKey;
        object["playback_ordinal"] = definition.playbackOrdinal;
        object["ornament_provenance"] = "score_declared";
        ornamentJson.append(object);
    }

    QJsonArray traversalJson;
    QJsonArray fileJson;
    for (const BuiltFile& file : files) {
        QJsonObject traversal;
        traversal["traversal_id"] = QString::fromStdString(file.source->traversalId);
        traversal["variant_kind"] = QString::fromStdString(midiKindName(file.source->kind));
        QJsonArray segments;
        for (const MidiExportedFile::TraversalSegment& segment : file.source->traversalSegments) {
            QJsonObject object;
            object["repeat_segment_id"] = QString::fromStdString(segment.repeatSegmentId);
            object["source_start_tick"] = segment.sourceStartTick;
            object["source_end_tick"] = segment.sourceEndTick;
            object["unfolded_start_tick"] = segment.unfoldedStartTick;
            object["traversal_ordinal"] = segment.traversalOrdinal;
            object["ending_id"] = nullableString(segment.endingId);
            segments.append(object);
        }
        traversal["segments"] = segments;
        traversalJson.append(traversal);

        QJsonObject output;
        output["kind"] = QString::fromStdString(midiKindName(file.source->kind));
        output["path"] = QString::fromStdString(manifestFileName(file.source->path));
        output["expand_repeats"] = file.source->expandRepeats;
        output["traversal_id"] = QString::fromStdString(file.source->traversalId);
        QJsonArray visualOccurrences;
        for (const BuiltOccurrence& occurrence : file.visualOccurrences) {
            visualOccurrences.append(occurrenceJson(occurrence, false));
        }
        output["visual_occurrences"] = visualOccurrences;
        QJsonArray ornamentOccurrences;
        for (const BuiltOccurrence& occurrence : file.ornamentOccurrences) {
            ornamentOccurrences.append(occurrenceJson(occurrence, true));
        }
        output["ornament_occurrences"] = ornamentOccurrences;
        QJsonArray audibleEvents;
        for (const BuiltAudibleEvent& audible : file.audibleEvents) {
            QJsonObject object;
            object["audible_event_id"] = QString::fromStdString(audible.audibleEventId);
            object["event_type"] = QString::fromStdString(audible.eventType);
            object["heard_midi_key"] = audible.source->heardMidiKey;
            object["idx"] = audible.idx >= 0 ? QJsonValue(audible.idx) : QJsonValue(QJsonValue::Null);
            QJsonArray occurrenceIds;
            for (const std::string& occurrenceId : audible.occurrenceIds) {
                occurrenceIds.append(QString::fromStdString(occurrenceId));
            }
            object["occurrence_ids"] = occurrenceIds;
            QJsonArray aliasIds;
            for (const std::string& aliasId : audible.aliasIds) {
                aliasIds.append(QString::fromStdString(aliasId));
            }
            object["alias_ids"] = aliasIds;
            object["ornament_definition_id"] = nullableString(audible.ornamentDefinitionId);
            object["ornament_provenance"] = audible.eventType == "nonvisual_ornament"
                                                   ? QJsonValue("score_declared") : QJsonValue(QJsonValue::Null);
            object["note_on_velocity"] = audible.source->noteOnVelocity;
            object["note_off_velocity"] = audible.source->noteOffVelocity;
            object["note_on_locator"] = locatorJson(audible.source->noteOn);
            object["note_off_locator"] = locatorJson(audible.source->noteOff);
            audibleEvents.append(object);
        }
        output["audible_events"] = audibleEvents;
        fileJson.append(output);
    }

    QJsonObject root;
    root["schema_version"] = compatibility::EXPORTER_PROVENANCE_SCHEMA;
    root["score"] = score;
    root["repeat_metadata"] = repeatMetadata;
    root["canonical_events"] = canonicalEvents;
    root["ornament_definitions"] = ornamentJson;
    root["traversals"] = traversalJson;
    root["files"] = fileJson;
    result.manifestJson = QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString() + "\n";
    return true;
}
}

RepeatExportInfo mu::project::pianomania::analyzeRepeatExportInfo(mu::engraving::Score* score)
{
    RepeatExportInfo info;
    if (!score) {
        return info;
    }

    const auto& repeatList = score->repeatList(true, false);
    info.hasRepeats = repeatList.size() > 1;

    std::set<int> endings;
    for (const auto& entry : score->spannerMap().map()) {
        const mu::engraving::Spanner* spanner = entry.second;
        if (!spanner || !spanner->isVolta() || !spanner->playSpanner()) {
            continue;
        }

        const mu::engraving::Volta* volta = mu::engraving::toVolta(spanner);
        for (int ending : volta->endings()) {
            endings.insert(ending);
        }
    }

    if (!endings.empty()) {
        info.hasRepeats = true;
        info.hasMultipleEndings = endings.size() > 1;
        info.finalEndingNumber = *endings.rbegin();
    }

    return info;
}

std::unique_ptr<mu::engraving::MasterScore> mu::project::pianomania::buildNoRepeatScoreForFinalEnding(mu::engraving::Score* score,
                                                                                                      int finalEndingNumber)
{
    if (!score || finalEndingNumber <= 0) {
        return nullptr;
    }

    auto* master = score->masterScore();
    if (!master) {
        return nullptr;
    }

    std::unique_ptr<mu::engraving::MasterScore> clone(master->clone());
    if (!clone) {
        return nullptr;
    }

    removeNonFinalEndings(clone.get(), finalEndingNumber);
    clone->setUpTempoMap();
    clone->setLayoutAll();
    clone->doLayout();
    return clone;
}

bool mu::project::pianomania::writeMidiFile(mu::engraving::Score* score, const muse::io::path_t& path, bool expandRepeats,
                                            bool exportRpns)
{
    if (!score) {
        return false;
    }

    mu::iex::midi::ExportMidi exportMidi(score);
    const mu::engraving::SynthesizerState synthState = score->synthesizerState();
    mu::engraving::MasterScore* master = score->masterScore();
    bool previousExpandRepeats = master ? master->expandRepeats() : false;

    bool ok = exportMidi.write(path.toQString(), expandRepeats, exportRpns, synthState);

    if (master) {
        master->setExpandRepeats(previousExpandRepeats);
    }

    if (!ok) {
        LOGE() << "Failed to export MIDI: " << path.toQString();
    }

    return ok;
}

muse::RetVal<PianomaniaExportResult> mu::project::pianomania::exportPianomaniaBundle(
    mu::engraving::Score* score,
    const muse::io::path_t& sourcePath,
    const muse::io::path_t& basePath,
    const muse::io::path_t& meiPath,
    bool exportRpns)
{
    muse::RetVal<PianomaniaExportResult> rv;
    if (!score || sourcePath.empty() || basePath.empty() || meiPath.empty()) {
        rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                std::string("Pianomania bundle requires score, source, base, and MEI paths"));
        return rv;
    }

    const std::string topologyError = validateVisibleScoreTopology(score);
    if (!topologyError.empty()) {
        rv.ret = muse::make_ret(muse::Ret::Code::UnknownError, topologyError);
        return rv;
    }

    rv.val.source = QFileInfo(sourcePath.toQString()).fileName().toStdString();
    rv.val.basePath = basePath;
    rv.val.meiPath = meiPath;
    rv.val.repeatInfo = analyzeRepeatExportInfo(score);

    std::string meiData;
    std::vector<mu::iex::mei::PmMeiNoteRecord> canonicalRecords;
    bool wroteCanonicalMei = false;
    {
        ScopedCanonicalNotePlayback canonicalNotePlayback(score);
        wroteCanonicalMei = mu::iex::mei::pmWriteMeiToString(score, true, meiData, &canonicalRecords);
    }
    if (!wroteCanonicalMei || !buildCanonicalEvents(canonicalRecords, rv.val.canonicalEvents)) {
        rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                std::string("Failed to build coordinated Pianomania MEI identities"));
        return rv;
    }
    std::unique_ptr<mu::engraving::MasterScore> noRepeatScore;
    std::vector<mu::iex::mei::PmMeiNoteRecord> noRepeatRecords;
    if (rv.val.repeatInfo.hasMultipleEndings && rv.val.repeatInfo.finalEndingNumber > 0) {
        noRepeatScore = buildNoRepeatScoreForFinalEnding(score, rv.val.repeatInfo.finalEndingNumber);
        if (!noRepeatScore) {
            rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                    std::string("Failed to build final-ending no-repeat score"));
            return rv;
        }
        std::string ignoredMei;
        bool wroteNoRepeatMei = false;
        {
            ScopedCanonicalNotePlayback canonicalNotePlayback(noRepeatScore.get());
            wroteNoRepeatMei = mu::iex::mei::pmWriteMeiToString(noRepeatScore.get(), false, ignoredMei, &noRepeatRecords);
        }
        if (!wroteNoRepeatMei) {
            rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                    std::string("Failed to resolve no-repeat canonical identities"));
            return rv;
        }
    }

    mu::engraving::Score* noRepeatExportScore = noRepeatScore ? noRepeatScore.get() : score;
    const std::vector<mu::iex::mei::PmMeiNoteRecord>& noRepeatVariantRecords = noRepeatScore ? noRepeatRecords : canonicalRecords;

    const auto writeVariant = [&](mu::engraving::Score* exportScore,
                                  const muse::io::path_t& path,
                                  bool expandRepeats,
                                  MidiExportedFile::Kind kind,
                                  const std::vector<mu::iex::mei::PmMeiNoteRecord>& canonicalVariantRecords) -> bool {
        mu::iex::midi::ExportedMidiProvenance provenance;
        const mu::engraving::SynthesizerState synthState = exportScore->synthesizerState();
        mu::engraving::MasterScore* master = exportScore->masterScore();
        const bool previousExpandRepeats = master ? master->expandRepeats() : false;
        bool written = false;
        {
            ScopedCanonicalNotePlayback canonicalNotePlayback(exportScore);
            mu::iex::midi::ExportMidi exporter(exportScore);
            written = exporter.write(path.toQString(), expandRepeats, exportRpns, synthState, &provenance);
        }
        if (master) {
            master->setExpandRepeats(previousExpandRepeats);
        }
        if (!written) {
            LOGE() << "Failed to write coordinated Pianomania MIDI: " << path.toQString();
            return false;
        }

        MidiExportedFile file;
        file.kind = kind;
        file.path = path;
        file.expandRepeats = expandRepeats;
        file.traversalId = midiKindName(kind);
        file.traversalSegments = traversalSegments(exportScore, file.traversalId, expandRepeats);
        file.variantCanonicalEvents = variantCanonicalEvents(canonicalVariantRecords);
        if (file.traversalSegments.empty() || file.variantCanonicalEvents.empty()
            || !stabilizeProvenance(provenance, file) || !verifyWrittenMidi(file)) {
            return false;
        }
        rv.val.files.push_back(std::move(file));
        return true;
    };

    // `midi` is the repeat-off gameplay traversal. Both MIDI variants use the
    // one canonical MEI; there is no third/legacy no-repeats MIDI role in v3.
    if (!writeVariant(noRepeatExportScore, basePath + ".mid", false, MidiExportedFile::Kind::Base,
                      noRepeatVariantRecords)) {
        rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                std::string("Failed to export base Pianomania variant"));
        return rv;
    }
    if (rv.val.repeatInfo.hasRepeats
        && !writeVariant(score, basePath + "-repeats.mid", true,
                         MidiExportedFile::Kind::Repeats, canonicalRecords)) {
        rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                std::string("Failed to export repeated Pianomania variant"));
        return rv;
    }

    if (!canonicalizePublicIdentityGraph(meiData, rv.val)) {
        rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                std::string("Failed to finalize coordinated Pianomania public identities"));
        return rv;
    }

    QFile meiFile(meiPath.toQString());
    if (!meiFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || meiFile.write(meiData.data(), static_cast<qint64>(meiData.size())) != static_cast<qint64>(meiData.size())) {
        rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                std::string("Failed to write coordinated Pianomania MEI"));
        return rv;
    }
    meiFile.close();

    std::set<std::string> canonicalIds;
    for (const CanonicalEvent& event : rv.val.canonicalEvents) {
        canonicalIds.insert(event.scoreEventId);
    }
    for (const MidiExportedFile& file : rv.val.files) {
        for (const MidiExportedFile::VariantCanonicalEvent& event : file.variantCanonicalEvents) {
            if (!canonicalIds.count(event.scoreEventId)) {
                rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                        std::string("Variant contains a noncanonical score event identity: ")
                                        + event.scoreEventId + ", variant=" + midiKindName(file.kind)
                                        + ", score_tick=" + std::to_string(event.scoreTick));
                return rv;
            }
        }
    }

    if (!buildManifestJson(rv.val)) {
        rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                std::string("Failed to assemble Pianomania export manifest v3"));
        return rv;
    }
    if (containsMuseScoreIdentity(rv.val.manifestJson)) {
        rv.ret = muse::make_ret(muse::Ret::Code::UnknownError,
                                std::string("Coordinated Pianomania manifest retains an internal identity"));
        return rv;
    }
    rv.ret = muse::make_ret(muse::Ret::Code::Ok);
    return rv;
}

bool mu::project::pianomania::writePianomaniaManifest(const muse::io::path_t& path,
                                                       const PianomaniaExportResult& result)
{
    if (path.empty() || result.manifestJson.empty()) {
        return false;
    }
    QFile file(path.toQString());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const qint64 expected = static_cast<qint64>(result.manifestJson.size());
    return file.write(result.manifestJson.data(), expected) == expected;
}
