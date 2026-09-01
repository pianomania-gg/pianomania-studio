/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2026 MuseScore Limited
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

#include "testing/qtestsuite.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <tuple>
#include <vector>

#include <QBuffer>

#include "testbase.h"

#include "engraving/dom/masterscore.h"
#include "importexport/midi/internal/midiexport/exportmidi.h"

using namespace mu::engraving;
using namespace mu::iex::midi;

namespace {
struct ExportedMidiNote {
    int channel = 0;
    int pitch = 0;
    int start = 0;
    int end = 0;
    int onVelocity = 0;
    int offVelocity = 0;

    int duration() const { return end - start; }
};

struct OpenNote {
    int start = 0;
    int velocity = 0;
};

static bool isNoteOnEvent(const MidiEvent& event)
{
    return event.type() == ME_NOTEON && event.velo() > 0;
}

static bool isNoteOffEvent(const MidiEvent& event)
{
    return event.type() == ME_NOTEOFF || (event.type() == ME_NOTEON && event.velo() == 0);
}

static int eventPriority(const MidiEvent& event)
{
    if (isNoteOffEvent(event)) {
        return 0;
    }
    if (isNoteOnEvent(event)) {
        return 2;
    }
    return 1;
}

static std::vector<ExportedMidiNote> collectMidiNotes(const MidiFile& midiFile)
{
    std::vector<ExportedMidiNote> notes;
    for (const MidiTrack& track : midiFile.tracks()) {
        std::vector<std::tuple<int, MidiEvent, size_t> > events;
        events.reserve(track.events().size());
        size_t sequence = 0;
        for (const auto& item : track.events()) {
            events.emplace_back(item.first, item.second, sequence++);
        }

        std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
            if (std::get<0>(left) != std::get<0>(right)) {
                return std::get<0>(left) < std::get<0>(right);
            }

            int leftPriority = eventPriority(std::get<1>(left));
            int rightPriority = eventPriority(std::get<1>(right));
            if (leftPriority != rightPriority) {
                return leftPriority < rightPriority;
            }

            return std::get<2>(left) < std::get<2>(right);
        });

        std::map<std::pair<int, int>, std::vector<OpenNote> > openNotes;
        for (const auto& item : events) {
            int tick = std::get<0>(item);
            const MidiEvent& event = std::get<1>(item);
            std::pair<int, int> key(event.channel(), event.pitch());

            if (isNoteOnEvent(event)) {
                openNotes[key].push_back({ tick, event.velo() });
            } else if (isNoteOffEvent(event)) {
                auto openIt = openNotes.find(key);
                if (openIt == openNotes.end() || openIt->second.empty()) {
                    continue;
                }

                OpenNote open = openIt->second.back();
                openIt->second.pop_back();
                if (openIt->second.empty()) {
                    openNotes.erase(openIt);
                }

                notes.push_back({ event.channel(), event.pitch(), open.start, tick, open.velocity, event.velo() });
            }
        }
    }

    std::sort(notes.begin(), notes.end(), [](const ExportedMidiNote& left, const ExportedMidiNote& right) {
        return std::tie(left.pitch, left.start, left.end, left.channel)
               < std::tie(right.pitch, right.start, right.end, right.channel);
    });
    return notes;
}

static std::vector<ExportedMidiNote> notesForPitch(const std::vector<ExportedMidiNote>& notes, int pitch)
{
    std::vector<ExportedMidiNote> result;
    std::copy_if(notes.begin(), notes.end(), std::back_inserter(result), [pitch](const ExportedMidiNote& note) {
        return note.pitch == pitch;
    });
    return result;
}

static std::vector<ExportedMidiNote> notesForPitchInRange(const std::vector<ExportedMidiNote>& notes, int pitch, int startTick,
                                                          int endTick)
{
    std::vector<ExportedMidiNote> result;
    std::copy_if(notes.begin(), notes.end(), std::back_inserter(result),
                 [pitch, startTick, endTick](const ExportedMidiNote& note) {
        return note.pitch == pitch && note.start >= startTick && note.start < endTick;
    });
    std::sort(result.begin(), result.end(), [](const ExportedMidiNote& left, const ExportedMidiNote& right) {
        return std::tie(left.start, left.end, left.pitch, left.channel)
               < std::tie(right.start, right.end, right.pitch, right.channel);
    });
    return result;
}

static std::vector<ExportedMidiNote> notesForPitches(const std::vector<ExportedMidiNote>& notes, const std::vector<int>& pitches)
{
    std::vector<ExportedMidiNote> result;
    std::copy_if(notes.begin(), notes.end(), std::back_inserter(result), [&pitches](const ExportedMidiNote& note) {
        return std::find(pitches.begin(), pitches.end(), note.pitch) != pitches.end();
    });
    std::sort(result.begin(), result.end(), [](const ExportedMidiNote& left, const ExportedMidiNote& right) {
        return std::tie(left.start, left.end, left.pitch, left.channel)
               < std::tie(right.start, right.end, right.pitch, right.channel);
    });
    return result;
}

static int countNotesWithOffVelocity(const std::vector<ExportedMidiNote>& notes, int offVelocity)
{
    return static_cast<int>(std::count_if(notes.begin(), notes.end(), [offVelocity](const ExportedMidiNote& note) {
        return note.offVelocity == offVelocity;
    }));
}

static bool hasBalancedNotePairs(const MidiFile& midiFile)
{
    for (const MidiTrack& track : midiFile.tracks()) {
        std::vector<std::tuple<int, MidiEvent, size_t> > events;
        events.reserve(track.events().size());
        size_t sequence = 0;
        for (const auto& item : track.events()) {
            events.emplace_back(item.first, item.second, sequence++);
        }

        std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
            if (std::get<0>(left) != std::get<0>(right)) {
                return std::get<0>(left) < std::get<0>(right);
            }

            int leftPriority = eventPriority(std::get<1>(left));
            int rightPriority = eventPriority(std::get<1>(right));
            if (leftPriority != rightPriority) {
                return leftPriority < rightPriority;
            }

            return std::get<2>(left) < std::get<2>(right);
        });

        std::map<std::pair<int, int>, int> openCounts;
        for (const auto& item : events) {
            const MidiEvent& event = std::get<1>(item);
            std::pair<int, int> key(event.channel(), event.pitch());
            if (isNoteOnEvent(event)) {
                ++openCounts[key];
            } else if (isNoteOffEvent(event)) {
                auto openIt = openCounts.find(key);
                if (openIt == openCounts.end() || openIt->second == 0) {
                    return false;
                }
                --openIt->second;
                if (openIt->second == 0) {
                    openCounts.erase(openIt);
                }
            }
        }
        if (!openCounts.empty()) {
            return false;
        }
    }

    return true;
}

static bool hasNoteOffBeforeNoteOnAtTick(const MidiFile& midiFile, int pitch, int tick)
{
    for (const MidiTrack& track : midiFile.tracks()) {
        auto range = track.events().equal_range(tick);
        bool sawNoteOff = false;
        for (auto it = range.first; it != range.second; ++it) {
            const MidiEvent& event = it->second;
            if (!isNoteOnEvent(event) && !isNoteOffEvent(event)) {
                continue;
            }
            if (event.pitch() != pitch) {
                continue;
            }
            if (isNoteOffEvent(event)) {
                sawNoteOff = true;
            } else if (isNoteOnEvent(event)) {
                return sawNoteOff;
            }
        }
    }

    return false;
}
}

class MidiExportTimingTests : public QObject, public MTest
{
    Q_OBJECT

private slots:
    void initTestCase();
    void staccatoNotesUseAdaptiveBand();
    void ordinarySixteenthStaccatoStaysShort();
    void heldPitchRestrikesCloseActiveNote();
    void overlappingSamePitchRestrikesInsideHeldChord();
    void samePitchAcciaccaturaRestrikesAfterPreviousNote();
    void measureBoundaryAcciaccaturaGetsDistinctStartTick();
    void exactSameStartUnisonsMergeToLongestDuration();
    void ordinaryNonTrillNotesStaySingleEvents();
    void basicTrillAlternatesWithSaneDurations();
    void fastTempoTrillAvoidsTinyArtifacts();
    void slowTempoTrillStaysDenseEnough();
    void tiedTrillContinuesAcrossTie();
    void trillBeforeFollowingNoteTerminatesCleanly();
    void writtenGraceBeforeOrnamentExportsMidiEvents();
    void afterGraceTrillNotesRemainCanonicalNotes();
    void chordTrillUsesExplicitUpperOrnamentAccidental();
    void mordentUsesExplicitLowerOrnamentAccidental();
    void turnUsesExplicitUpperAndLowerOrnamentAccidentals();
    void trillLineUsesExplicitUpperOrnamentAccidental();
    void shortTrillMarksOnlySustainedWrittenPitchAsLayoutNote();

private:
    bool exportFixture(const QString& fixtureName, MidiFile& midiFile, std::vector<ExportedMidiNote>& notes);
};

void MidiExportTimingTests::initTestCase()
{
    setRootDir(QString(iex_midi_tests_DATA_ROOT));
}

bool MidiExportTimingTests::exportFixture(const QString& fixtureName, MidiFile& midiFile, std::vector<ExportedMidiNote>& notes)
{
    MasterScore* score = readScore(QString("midiexport_data/") + fixtureName);
    if (!score) {
        return false;
    }

    QBuffer buffer;
    bool bufferOpen = buffer.open(QIODevice::ReadWrite);
    if (!bufferOpen) {
        delete score;
        return false;
    }

    ExportMidi exportMidi(score);
    bool writeOk = exportMidi.write(&buffer, true, true);
    delete score;
    if (!writeOk) {
        return false;
    }

    buffer.seek(0);
    try {
        if (!midiFile.read(&buffer)) {
            return false;
        }
    } catch (...) {
        return false;
    }

    notes = collectMidiNotes(midiFile);
    return true;
}

void MidiExportTimingTests::staccatoNotesUseAdaptiveBand()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_staccato_floor.mscx", midiFile, notes));

    std::vector<ExportedMidiNote> c4 = notesForPitch(notes, 60);
    std::vector<ExportedMidiNote> d4 = notesForPitch(notes, 62);
    std::vector<ExportedMidiNote> e4 = notesForPitch(notes, 64);
    std::vector<ExportedMidiNote> f4 = notesForPitch(notes, 65);

    QCOMPARE(static_cast<int>(c4.size()), 1);
    QCOMPARE(static_cast<int>(d4.size()), 1);
    QCOMPARE(static_cast<int>(e4.size()), 1);
    QCOMPARE(static_cast<int>(f4.size()), 1);

    QCOMPARE(c4.front().duration(), 120);
    QCOMPARE(d4.front().duration(), 60);
    QCOMPARE(e4.front().duration(), 30);
    QCOMPARE(f4.front().duration(), 3);
}

void MidiExportTimingTests::ordinarySixteenthStaccatoStaysShort()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_staccato_16th_contrast.mscx", midiFile, notes));

    std::vector<ExportedMidiNote> staccato = notesForPitch(notes, 60);
    std::vector<ExportedMidiNote> ordinary = notesForPitch(notes, 62);

    QCOMPARE(static_cast<int>(staccato.size()), 1);
    QCOMPARE(static_cast<int>(ordinary.size()), 1);

    QCOMPARE(staccato.front().duration(), 42);
    QVERIFY(ordinary.front().duration() > staccato.front().duration() * 2);
}

void MidiExportTimingTests::heldPitchRestrikesCloseActiveNote()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_held_pitch_restrike.mscx", midiFile, notes));

    std::vector<ExportedMidiNote> fSharp4 = notesForPitch(notes, 66);
    QCOMPARE(static_cast<int>(fSharp4.size()), 3);

    QCOMPARE(fSharp4[0].start, 0);
    QCOMPARE(fSharp4[0].end, 480);
    QCOMPARE(fSharp4[1].start, 480);
    QCOMPARE(fSharp4[1].duration(), 479);
    QCOMPARE(fSharp4[2].start, 960);
    QCOMPARE(fSharp4[2].duration(), 479);
    QVERIFY(hasNoteOffBeforeNoteOnAtTick(midiFile, 66, 480));
}

void MidiExportTimingTests::overlappingSamePitchRestrikesInsideHeldChord()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_overlapping_same_pitch_restrike.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> e4 = notesForPitch(notes, 64);
    std::vector<ExportedMidiNote> g4 = notesForPitch(notes, 67);
    std::vector<ExportedMidiNote> d5 = notesForPitch(notes, 74);
    QCOMPARE(static_cast<int>(e4.size()), 2);
    QCOMPARE(static_cast<int>(g4.size()), 2);
    QCOMPARE(static_cast<int>(d5.size()), 3);

    QCOMPARE(e4[0].start, 0);
    QCOMPARE(e4[0].end, 360);
    QCOMPARE(e4[0].offVelocity, 64);
    QCOMPARE(e4[1].start, 360);
    QCOMPARE(e4[1].duration(), 119);
    QCOMPARE(e4[1].offVelocity, 64);

    QCOMPARE(g4[0].start, 0);
    QCOMPARE(g4[0].end, 240);
    QCOMPARE(g4[0].offVelocity, 64);
    QCOMPARE(g4[1].start, 240);
    QCOMPARE(g4[1].duration(), 119);
    QCOMPARE(g4[1].offVelocity, 64);

    QCOMPARE(d5[0].start, 0);
    QCOMPARE(d5[0].end, 120);
    QCOMPARE(d5[0].offVelocity, 64);
    QCOMPARE(d5[1].start, 120);
    QCOMPARE(d5[1].duration(), 119);
    QCOMPARE(d5[1].offVelocity, 64);
    QCOMPARE(d5[2].start, 240);
    QCOMPARE(d5[2].duration(), 119);
    QCOMPARE(d5[2].offVelocity, 64);

    QVERIFY(hasNoteOffBeforeNoteOnAtTick(midiFile, 74, 120));
    QVERIFY(hasNoteOffBeforeNoteOnAtTick(midiFile, 67, 240));
    QVERIFY(hasNoteOffBeforeNoteOnAtTick(midiFile, 64, 360));
}

void MidiExportTimingTests::samePitchAcciaccaturaRestrikesAfterPreviousNote()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_acciaccatura_same_pitch_restrike.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> e4 = notesForPitch(notes, 64);
    std::vector<ExportedMidiNote> f4 = notesForPitch(notes, 65);
    QCOMPARE(static_cast<int>(e4.size()), 2);
    QCOMPARE(static_cast<int>(f4.size()), 1);

    QCOMPARE(e4[0].start, 0);
    QCOMPARE(e4[0].end, 119);
    QCOMPARE(e4[0].offVelocity, 64);
    QCOMPARE(e4[1].start, 120);
    QCOMPARE(e4[1].end, 239);
    QCOMPARE(e4[1].offVelocity, 64);
    QCOMPARE(f4.front().start, 240);
    QCOMPARE(f4.front().offVelocity, 64);
    QCOMPARE(e4[0].end, e4[1].start - 1);
}

void MidiExportTimingTests::measureBoundaryAcciaccaturaGetsDistinctStartTick()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_acciaccatura_measure_boundary_same_pitch.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> e4 = notesForPitch(notes, 64);
    std::vector<ExportedMidiNote> f4 = notesForPitch(notes, 65);
    QCOMPARE(static_cast<int>(e4.size()), 2);
    QCOMPARE(static_cast<int>(f4.size()), 1);

    QCOMPARE(e4[0].start, 1200);
    QCOMPARE(e4[0].end, 1319);
    QCOMPARE(e4[0].offVelocity, 64);
    QCOMPARE(e4[1].start, 1320);
    QCOMPARE(e4[1].end, 1439);
    QCOMPARE(e4[1].offVelocity, 64);
    QCOMPARE(f4.front().start, 1440);
    QCOMPARE(f4.front().offVelocity, 64);
}

void MidiExportTimingTests::exactSameStartUnisonsMergeToLongestDuration()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_same_start_unison.mscx", midiFile, notes));

    std::vector<ExportedMidiNote> c4 = notesForPitch(notes, 60);
    QCOMPARE(static_cast<int>(c4.size()), 1);
    QCOMPARE(c4.front().start, 0);
    QCOMPARE(c4.front().end, 959);
}

void MidiExportTimingTests::ordinaryNonTrillNotesStaySingleEvents()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_trill_ordinary_notes.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> c4 = notesForPitch(notes, 60);
    std::vector<ExportedMidiNote> d4 = notesForPitch(notes, 62);

    QCOMPARE(static_cast<int>(c4.size()), 1);
    QCOMPARE(static_cast<int>(d4.size()), 1);
    QCOMPARE(c4.front().start, 0);
    QCOMPARE(c4.front().end, 479);
    QCOMPARE(d4.front().start, 480);
    QCOMPARE(d4.front().end, 959);
}

void MidiExportTimingTests::basicTrillAlternatesWithSaneDurations()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_trill_basic_line.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> trillNotes = notesForPitches(notes, { 60, 62 });
    QVERIFY(static_cast<int>(trillNotes.size()) >= 20);
    QVERIFY(static_cast<int>(trillNotes.size()) <= 28);

    for (size_t i = 1; i < std::min<size_t>(trillNotes.size(), 10); ++i) {
        QVERIFY(trillNotes[i].start > trillNotes[i - 1].start);
        QVERIFY(trillNotes[i].pitch != trillNotes[i - 1].pitch);
    }

    bool sawOrnamentNoteOff = false;
    for (const ExportedMidiNote& note : trillNotes) {
        QVERIFY(note.duration() >= 60);
        QVERIFY(note.duration() <= 140);
        if (note.pitch == 62 && note.offVelocity == 127) {
            sawOrnamentNoteOff = true;
        }
    }
    QVERIFY(sawOrnamentNoteOff);
}

void MidiExportTimingTests::fastTempoTrillAvoidsTinyArtifacts()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_trill_fast_tempo.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> trillNotes = notesForPitches(notes, { 60, 62 });
    QVERIFY(static_cast<int>(trillNotes.size()) >= 10);
    QVERIFY(static_cast<int>(trillNotes.size()) <= 16);

    for (const ExportedMidiNote& note : trillNotes) {
        QVERIFY(note.duration() >= 120);
    }
}

void MidiExportTimingTests::slowTempoTrillStaysDenseEnough()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_trill_slow_tempo.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> trillNotes = notesForPitches(notes, { 60, 62 });
    QVERIFY(static_cast<int>(trillNotes.size()) >= 70);
    QVERIFY(static_cast<int>(trillNotes.size()) <= 90);

    for (const ExportedMidiNote& note : trillNotes) {
        QVERIFY(note.duration() >= 20);
    }
}

void MidiExportTimingTests::tiedTrillContinuesAcrossTie()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_trill_tied_span.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> trillNotes = notesForPitches(notes, { 60, 62 });
    QVERIFY(static_cast<int>(trillNotes.size()) >= 42);
    QVERIFY(static_cast<int>(trillNotes.size()) <= 54);

    int latestEnd = 0;
    for (const ExportedMidiNote& note : trillNotes) {
        latestEnd = std::max(latestEnd, note.end);
        QVERIFY(note.end < 3840);
    }
    QVERIFY(latestEnd > 3000);
}

void MidiExportTimingTests::trillBeforeFollowingNoteTerminatesCleanly()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_trill_before_following_note.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> d4 = notesForPitch(notes, 62);
    auto following = std::find_if(d4.begin(), d4.end(), [](const ExportedMidiNote& note) {
        return note.start == 960;
    });
    QVERIFY(following != d4.end());
    QCOMPARE(following->duration(), 479);

    for (const ExportedMidiNote& note : d4) {
        if (note.start < 960) {
            QVERIFY(note.end < 960);
        }
    }
}

void MidiExportTimingTests::writtenGraceBeforeOrnamentExportsMidiEvents()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_grace_ornament_accidental_export.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> g4Grace = notesForPitchInRange(notes, 67, 0, 480);
    std::vector<ExportedMidiNote> a4Grace = notesForPitchInRange(notes, 69, 0, 480);
    QCOMPARE(static_cast<int>(g4Grace.size()), 1);
    QCOMPARE(static_cast<int>(a4Grace.size()), 1);
    QVERIFY(g4Grace.front().start < 480);
    QVERIFY(a4Grace.front().start < 480);
    QCOMPARE(g4Grace.front().offVelocity, 64);
    QCOMPARE(a4Grace.front().offVelocity, 64);
}

void MidiExportTimingTests::afterGraceTrillNotesRemainCanonicalNotes()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_trill_after_grace_canonical.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> c4 = notesForPitchInRange(notes, 60, 0, 1920);
    std::vector<ExportedMidiNote> d4 = notesForPitchInRange(notes, 62, 0, 1920);

    QCOMPARE(countNotesWithOffVelocity(c4, 64), 2);
    QCOMPARE(countNotesWithOffVelocity(d4, 64), 1);
    QVERIFY(countNotesWithOffVelocity(c4, 127) >= 1);
    QVERIFY(countNotesWithOffVelocity(d4, 127) >= 1);
}

void MidiExportTimingTests::chordTrillUsesExplicitUpperOrnamentAccidental()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_grace_ornament_accidental_export.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> cSharp4 = notesForPitchInRange(notes, 61, 480, 960);
    std::vector<ExportedMidiNote> dNatural4 = notesForPitchInRange(notes, 62, 480, 960);
    QVERIFY(static_cast<int>(cSharp4.size()) >= 1);
    QCOMPARE(static_cast<int>(dNatural4.size()), 0);
}

void MidiExportTimingTests::mordentUsesExplicitLowerOrnamentAccidental()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_grace_ornament_accidental_export.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> cSharp4 = notesForPitchInRange(notes, 61, 1920, 2400);
    std::vector<ExportedMidiNote> cNatural4 = notesForPitchInRange(notes, 60, 1920, 2400);
    QCOMPARE(static_cast<int>(cSharp4.size()), 1);
    QCOMPARE(static_cast<int>(cNatural4.size()), 0);
}

void MidiExportTimingTests::turnUsesExplicitUpperAndLowerOrnamentAccidentals()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_grace_ornament_accidental_export.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> eFlat4 = notesForPitchInRange(notes, 63, 3840, 4320);
    std::vector<ExportedMidiNote> cSharp4 = notesForPitchInRange(notes, 61, 3840, 4320);
    std::vector<ExportedMidiNote> eNatural4 = notesForPitchInRange(notes, 64, 3840, 4320);
    std::vector<ExportedMidiNote> cNatural4 = notesForPitchInRange(notes, 60, 3840, 4320);
    QCOMPARE(static_cast<int>(eFlat4.size()), 1);
    QCOMPARE(static_cast<int>(cSharp4.size()), 1);
    QCOMPARE(static_cast<int>(eNatural4.size()), 0);
    QCOMPARE(static_cast<int>(cNatural4.size()), 0);
}

void MidiExportTimingTests::trillLineUsesExplicitUpperOrnamentAccidental()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_grace_ornament_accidental_export.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> eFlat4 = notesForPitchInRange(notes, 63, 5760, 7680);
    std::vector<ExportedMidiNote> eNatural4 = notesForPitchInRange(notes, 64, 5760, 7680);
    QVERIFY(static_cast<int>(eFlat4.size()) >= 4);
    QCOMPARE(static_cast<int>(eNatural4.size()), 0);
}

void MidiExportTimingTests::shortTrillMarksOnlySustainedWrittenPitchAsLayoutNote()
{
    MidiFile midiFile;
    std::vector<ExportedMidiNote> notes;
    QVERIFY(exportFixture("pianomania_grace_ornament_accidental_export.mscx", midiFile, notes));
    QVERIFY(hasBalancedNotePairs(midiFile));

    std::vector<ExportedMidiNote> gSharp4 = notesForPitchInRange(notes, 68, 9600, 10080);
    std::vector<ExportedMidiNote> a4 = notesForPitchInRange(notes, 69, 9600, 10080);
    QCOMPARE(static_cast<int>(gSharp4.size()), 2);
    QCOMPARE(static_cast<int>(a4.size()), 1);

    QCOMPARE(gSharp4[0].offVelocity, 127);
    QCOMPARE(a4.front().offVelocity, 127);
    QCOMPARE(gSharp4[1].offVelocity, 64);
    QVERIFY(gSharp4[1].duration() > gSharp4[0].duration());
}

QTEST_MAIN(MidiExportTimingTests)

#include "midiexport_timing_tests.moc"
