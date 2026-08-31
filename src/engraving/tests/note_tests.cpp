/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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

#include <gtest/gtest.h>

#include <algorithm>

#include "engraving/compat/scoreaccess.h"
#include "engraving/dom/accidental.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/pitchspelling.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/tremolosinglechord.h"

#include "utils/scorerw.h"
#include "utils/scorecomp.h"

using namespace mu::engraving;

static const String NOTE_DATA_DIR("note_data/");

class Engraving_NoteTests : public ::testing::Test
{
};

//---------------------------------------------------------
///   note
///   read/write test of note
//---------------------------------------------------------

TEST_F(Engraving_NoteTests, note)
{
    MasterScore* score = compat::ScoreAccess::createMasterScore(nullptr);
    Chord* chord = Factory::createChord(score->dummy()->segment());
    Note* note = Factory::createNote(chord);
    chord->add(note);

    // pitch
    note->setPitch(33);
    note->setTpcFromPitch();
    Note* n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->pitch(), 33);
    delete n;

    // tpc
    note->setTpc1(22);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->tpc1(), 22);
    delete n;

    note->setTpc1(23);
    note->setTpc2(23);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->tpc2(), 23);
    delete n;

    note->setPianomaniaHeldNote(true);
    note->setPianomaniaHeldNotePulse(Note::PianomaniaHeldNotePulse::Eighth);
    note->setPianomaniaHeldNotePulseTriplet(true);
    const PianomaniaHeldNotePitchCurve heldPitchCurve { { 0, 0, 0 }, { 120, -200, -1200 }, { 480, -1200, 0 } };
    note->setPianomaniaHeldNotePitchCurve(heldPitchCurve);
    note->setPianomaniaHand(Note::PianomaniaHand::Right);
    note->setPianomaniaShakeNote(true);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_TRUE(n->pianomaniaHeldNote());
    EXPECT_EQ(n->pianomaniaHeldNotePulse(), Note::PianomaniaHeldNotePulse::Eighth);
    EXPECT_TRUE(n->pianomaniaHeldNotePulseTriplet());
    EXPECT_EQ(n->pianomaniaHeldNotePitchCurve(), heldPitchCurve);
    EXPECT_EQ(n->pianomaniaHand(), Note::PianomaniaHand::Right);
    EXPECT_TRUE(n->pianomaniaShakeNote());
    delete n;

    Note* clonedNote = note->clone();
    EXPECT_TRUE(clonedNote->pianomaniaHeldNote());
    EXPECT_EQ(clonedNote->pianomaniaHeldNotePulse(), Note::PianomaniaHeldNotePulse::Eighth);
    EXPECT_TRUE(clonedNote->pianomaniaHeldNotePulseTriplet());
    EXPECT_EQ(clonedNote->pianomaniaHeldNotePitchCurve(), heldPitchCurve);
    EXPECT_EQ(clonedNote->pianomaniaHand(), Note::PianomaniaHand::Right);
    EXPECT_TRUE(clonedNote->pianomaniaShakeNote());
    delete clonedNote;

    // small
    note->setSmall(true);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_TRUE(n->isSmall());
    delete n;

    // mirror
    note->setUserMirror(DirectionH::LEFT);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userMirror(), DirectionH::LEFT);
    delete n;

    note->setUserMirror(DirectionH::RIGHT);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userMirror(), DirectionH::RIGHT);
    delete n;

    note->setUserMirror(DirectionH::AUTO);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userMirror(), DirectionH::AUTO);
    delete n;

    // dot position
    note->setUserDotPosition(DirectionV::UP);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(int(n->userDotPosition()), int(DirectionV::UP));
    delete n;

    note->setUserDotPosition(DirectionV::DOWN);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(int(n->userDotPosition()), int(DirectionV::DOWN));
    delete n;

    note->setUserDotPosition(DirectionV::AUTO);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(int(n->userDotPosition()), int(DirectionV::AUTO));
    delete n;
    // headGroup
    for (int i = 0; i < int(NoteHeadGroup::HEAD_GROUPS); ++i) {
        note->setHeadGroup(NoteHeadGroup(i));
        n = toNote(ScoreRW::writeReadElement(note));
        EXPECT_EQ(int(n->headGroup()), i);
        delete n;
    }

    // headType
    for (int i = 0; i < int(NoteHeadType::HEAD_TYPES); ++i) {
        note->setHeadType(NoteHeadType(i));
        n = toNote(ScoreRW::writeReadElement(note));
        EXPECT_EQ(int(n->headType()), i);
        delete n;
    }

    // user velocity
    note->setUserVelocity(71);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userVelocity(), 71);
    delete n;

    // tuning
    note->setTuning(1.3);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->tuning(), 1.3);
    delete n;

    // fret
    note->setFret(9);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->fret(), 9);
    delete n;

    // string
    note->setString(3);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->string(), 3);
    delete n;

    // ghost
    note->setGhost(true);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_TRUE(n->ghost());
    delete n;

    //================================================
    //   test setProperty(int, QVariant)
    //================================================

    // pitch
    note->setProperty(Pid::PITCH, 32);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->pitch(), 32);
    delete n;

    // tpc
    note->setProperty(Pid::TPC1, 21);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->tpc1(), 21);
    delete n;

    note->setProperty(Pid::TPC1, 22);
    note->setProperty(Pid::TPC2, 22);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->tpc2(), 22);
    delete n;

    // small
    note->setProperty(Pid::SMALL, false);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_TRUE(!n->isSmall());
    delete n;

    note->setProperty(Pid::SMALL, true);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_TRUE(n->isSmall());
    delete n;

    // mirror
    note->setProperty(Pid::MIRROR_HEAD, DirectionH::LEFT);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userMirror(), DirectionH::LEFT);
    delete n;

    note->setProperty(Pid::MIRROR_HEAD, DirectionH::RIGHT);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userMirror(), DirectionH::RIGHT);
    delete n;

    note->setProperty(Pid::MIRROR_HEAD, DirectionH::AUTO);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userMirror(), DirectionH::AUTO);
    delete n;

    // dot position
    note->setProperty(Pid::DOT_POSITION, PropertyValue::fromValue(DirectionV::UP));
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userDotPosition(), DirectionV::UP);
    delete n;

    note->setProperty(Pid::DOT_POSITION, PropertyValue::fromValue(DirectionV::DOWN));
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userDotPosition(), DirectionV::DOWN);
    delete n;

    note->setProperty(Pid::DOT_POSITION, PropertyValue::fromValue(DirectionV::AUTO));
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userDotPosition(), DirectionV::AUTO);
    delete n;

    // headGroup
    for (int i = 0; i < int(NoteHeadGroup::HEAD_GROUPS); ++i) {
        note->setProperty(Pid::HEAD_GROUP, static_cast<NoteHeadGroup>(i));
        n = toNote(ScoreRW::writeReadElement(note));
        EXPECT_EQ(int(n->headGroup()), i);
        delete n;
    }

    // headType
    for (int i = 0; i < int(NoteHeadType::HEAD_TYPES); ++i) {
        note->setProperty(Pid::HEAD_TYPE, static_cast<NoteHeadType>(i));
        n = toNote(ScoreRW::writeReadElement(note));
        EXPECT_EQ(int(n->headType()), i);
        delete n;
    }

    // user velocity
    note->setProperty(Pid::USER_VELOCITY, 38);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->userVelocity(), 38);
    delete n;

    // tuning
    note->setProperty(Pid::TUNING, 2.4);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->tuning(), 2.4);
    delete n;

    // fret
    note->setProperty(Pid::FRET, 7);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->fret(), 7);
    delete n;

    // string
    note->setProperty(Pid::STRING, 4);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_EQ(n->string(), 4);
    delete n;

    // ghost
    note->setProperty(Pid::GHOST, false);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_TRUE(!n->ghost());
    delete n;

    note->setProperty(Pid::GHOST, true);
    n = toNote(ScoreRW::writeReadElement(note));
    EXPECT_TRUE(n->ghost());
    delete n;

    delete chord;

    delete score;
}

TEST_F(Engraving_NoteTests, PianomaniaHeldNotePitchCurveValidation)
{
    EXPECT_TRUE(Note::isValidPianomaniaHeldNotePitchCurve({}));
    EXPECT_TRUE(Note::isValidPianomaniaHeldNotePitchCurve(
        { { 0, 0, 0 }, { 120, -200, -1200 }, { 480, -1200, 0 } }, 480));
    EXPECT_FALSE(Note::isValidPianomaniaHeldNotePitchCurve({ { 0, 0, 0 } }));
    EXPECT_FALSE(Note::isValidPianomaniaHeldNotePitchCurve({ { 0, 10, 0 }, { 480, 100, 0 } }, 480));
    EXPECT_FALSE(Note::isValidPianomaniaHeldNotePitchCurve(
        { { 0, 0, 0 }, { 120, 0, 0 }, { 120, 100, 0 }, { 480, 100, 0 } }, 480));
    EXPECT_FALSE(Note::isValidPianomaniaHeldNotePitchCurve({ { 0, 0, 0 }, { 480, 2401, 0 } }, 480));
    EXPECT_FALSE(Note::isValidPianomaniaHeldNotePitchCurve({ { 0, 0, 0 }, { 479, 100, 0 } }, 480));
    EXPECT_FALSE(Note::isValidPianomaniaHeldNotePitchCurve({ { 0, 0, 0 }, { 480, 0, 0 } }, 480));
    EXPECT_FALSE(Note::isValidPianomaniaHeldNotePitchCurve({ { 0, 0, 1000001 }, { 480, 100, 0 } }, 480));
    EXPECT_FALSE(Note::isValidPianomaniaHeldNotePitchCurve({ { 0, 0, 100000 }, { 480, 2400, 100000 } }, 480));
    EXPECT_TRUE(Note::isValidPianomaniaHeldNotePitchCurve(
        { { 0, 0, 0 }, { 240, 2000, 100 }, { 480, 0, 0 } }, 480));
}

//---------------------------------------------------------
///   grace
///   read/write test of grace notes
//---------------------------------------------------------

TEST_F(Engraving_NoteTests, grace)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + u"grace.mscx");
    score->doLayout();
    Chord* chord = score->firstMeasure()->findChord(Fraction(0, 1), 0);
    Note* note = chord->upNote();

    // create
    score->setGraceNote(chord, note->pitch(), NoteType::APPOGGIATURA, Constants::DIVISION / 2);
    Chord* gc = chord->graceNotes().front();
    Note* gn = gc->notes().front();
//      Note* n = toNote(ScoreRW::writeReadElement(gn));
//      QCOMPARE(n->noteType(), NoteType::APPOGGIATURA);
//      delete n;

    // tie
    score->select(gn);
    score->cmdAddTie();
//      n = toNote(ScoreRW::writeReadElement(gn));
//      QVERIFY(n->tieFor() != 0);
//      delete n;

    // tremolo
    score->startCmd(TranslatableString::untranslatable("Engraving note tests"));
    TremoloSingleChord* tr = Factory::createTremoloSingleChord(gc);
    tr->setTremoloType(TremoloType::R16);
    tr->setParent(gc);
    tr->setTrack(gc->track());
    score->undoAddElement(tr);
    score->endCmd();
//      Chord* c = toChord(ScoreRW::writeReadElement(gc));
//      QVERIFY(c->tremolo() != 0);
//      delete c;

    // articulation
    score->startCmd(TranslatableString::untranslatable("Engraving note tests"));
    Articulation* ar = Factory::createArticulation(gc);
    ar->setSymId(SymId::articAccentAbove);
    ar->setParent(gc);
    ar->setTrack(gc->track());
    score->undoAddElement(ar);
    score->endCmd();
//      c = toChord(ScoreRW::writeReadElement(gc));
//      QVERIFY(c->articulations().size() == 1);
//      delete c;

    EXPECT_TRUE(ScoreComp::saveCompareScore(score, u"grace-test.mscx", NOTE_DATA_DIR + u"grace-ref.mscx"));
}

//---------------------------------------------------------
///   graceSlashSave
///   read/write test of grace notes
//---------------------------------------------------------

TEST_F(Engraving_NoteTests, graceAfterSlashSave)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + u"grace.mscx");
    score->doLayout();
    Chord* chord = score->firstMeasure()->findChord(Fraction(0, 1), 0);
    Note* note = chord->upNote();

    // create
    score->setGraceNote(chord, note->pitch(), NoteType::GRACE8_AFTER, Constants::DIVISION / 2);
    Chord* gc = chord->graceNotes().front();
    gc->undoChangeProperty(Pid::SHOW_STEM_SLASH, true);

    EXPECT_TRUE(gc->showStemSlash());

    EXPECT_TRUE(ScoreComp::saveCompareScore(score, u"graceAfterSlashSave-test.mscx", NOTE_DATA_DIR + u"graceAfterSlashSave-ref.mscx"));
}

//---------------------------------------------------------
///   tpc
///   test of note tpc values
//---------------------------------------------------------

TEST_F(Engraving_NoteTests, tpc)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + u"tpc.mscx");

    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_QUARTER);
    score->inputState().setNoteEntryMode(true);
    int octave = 5 * 7;
    score->cmdAddPitch(octave + 1, false, false);
    score->cmdAddPitch(octave + 2, false, false);
    score->cmdAddPitch(octave + 3, false, false);
    score->cmdAddPitch(octave + 4, false, false);
    score->cmdAddPitch(octave + 5, false, false);
    score->cmdAddPitch(octave + 6, false, false);
    score->cmdAddPitch(octave + 7, false, false);
    score->cmdAddPitch(octave + 8, false, false);

    score->cmdConcertPitchChanged(true);

    EXPECT_TRUE(ScoreComp::saveCompareScore(score, u"tpc-test.mscx", NOTE_DATA_DIR + u"tpc-ref.mscx"));
}

//---------------------------------------------------------
///   tpcTranspose
///   test of note tpc values & transposition
//---------------------------------------------------------

TEST_F(Engraving_NoteTests, tpcTranspose)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + u"tpc-transpose.mscx");

    score->startCmd(TranslatableString::untranslatable("Engraving note tests"));
    Measure* m = score->firstMeasure();
    score->select(m, SelectType::SINGLE, 0);
    score->changeAccidental(AccidentalType::FLAT);
    score->endCmd();

    score->startCmd(TranslatableString::untranslatable("Engraving note tests"));
    m = m->nextMeasure();
    score->select(m, SelectType::SINGLE, 0);
    score->upDown(false, UpDownMode::CHROMATIC);
    score->endCmd();

    score->startCmd(TranslatableString::untranslatable("Engraving note tests"));
    score->cmdConcertPitchChanged(true);
    score->endCmd();

    EXPECT_TRUE(ScoreComp::saveCompareScore(score, u"tpc-transpose-test.mscx", NOTE_DATA_DIR + u"tpc-transpose-ref.mscx"));
}

//---------------------------------------------------------
///   tpcTranspose2
///   more tests of note tpc values & transposition
//---------------------------------------------------------

TEST_F(Engraving_NoteTests, tpcTranspose2)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + u"tpc-transpose2.mscx");

    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_QUARTER);
    score->inputState().setNoteEntryMode(true);
    int octave = 5 * 7;
    score->cmdAddPitch(octave + 3, false, false);

    score->startCmd(TranslatableString::untranslatable("Engraving note tests"));
    score->cmdConcertPitchChanged(true);
    score->endCmd();

    printf("================\n");

    EXPECT_TRUE(ScoreComp::saveCompareScore(score, u"tpc-transpose2-test.mscx", NOTE_DATA_DIR + u"tpc-transpose2-ref.mscx"));
}

//---------------------------------------------------------
///   noteLimits
//---------------------------------------------------------

TEST_F(Engraving_NoteTests, noteLimits)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + u"empty.mscx");

    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_QUARTER);
    score->inputState().setNoteEntryMode(true);

    // over 127 shouldn't crash
    score->cmdAddPitch(140, false, false);
    // below 0 shouldn't crash
    score->cmdAddPitch(-40, false, false);

    // stack chords
    score->cmdAddPitch(42, false, false);
    for (int i = 1; i < 20; i++) {
        score->cmdAddPitch(42 + i * 7, true, false);
    }

    // interval below
    score->cmdAddPitch(42, false, false);
    for (int i = 0; i < 20; i++) {
        std::vector<Note*> nl = score->selection().noteList();
        score->startCmd(TranslatableString::untranslatable("Engraving note tests"));
        score->addInterval(-8, nl);
        score->endCmd();
    }

    // interval above
    score->cmdAddPitch(42, false, false);
    for (int i = 0; i < 20; i++) {
        std::vector<Note*> nl = score->selection().noteList();
        score->startCmd(TranslatableString::untranslatable("Engraving note tests"));
        score->addInterval(8, nl);
        score->endCmd();
    }
    EXPECT_TRUE(ScoreComp::saveCompareScore(score, u"notelimits-test.mscx", NOTE_DATA_DIR + u"notelimits-ref.mscx"));
}

TEST_F(Engraving_NoteTests, tpcDegrees)
{
    EXPECT_EQ(tpc2degree(Tpc::TPC_C,   Key::C),   0);
    //QCOMPARE(tpc2degree(Tpc::TPC_E_S, Key::C),   3);
    EXPECT_EQ(tpc2degree(Tpc::TPC_B,   Key::C),   6);
    EXPECT_EQ(tpc2degree(Tpc::TPC_F_S, Key::C_S), 3);
    EXPECT_EQ(tpc2degree(Tpc::TPC_B,   Key::C_S), 6);
    EXPECT_EQ(tpc2degree(Tpc::TPC_B_B, Key::C_S), 6);
    //QCOMPARE(tpc2degree(Tpc::TPC_B_S, Key::C_S), 7);
}

TEST_F(Engraving_NoteTests, alteredUnison)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + u"altered-unison.mscx");
    Measure* m = score->firstMeasure();
    Chord* c = m->findChord(Fraction(0, 1), 0);
    EXPECT_TRUE(c->downNote()->accidental() && c->downNote()->accidental()->accidentalType() == AccidentalType::FLAT);
    EXPECT_TRUE(c->upNote()->accidental() && c->upNote()->accidental()->accidentalType() == AccidentalType::NATURAL);
    c = m->findChord(Fraction(1, 4), 0);
    EXPECT_TRUE(c->downNote()->accidental() && c->downNote()->accidental()->accidentalType() == AccidentalType::NATURAL);
    EXPECT_TRUE(c->upNote()->accidental() && c->upNote()->accidental()->accidentalType() == AccidentalType::SHARP);
}

//---------------------------------------------------------
///   LongNoteAfterShort_183746
///    Put a small 128th rest
///    Then put a long Breve note
///    This breve will get spread out across multiple measures
///    Verifies that the resulting notes are tied over at least 3 times (to span 3 measures) and have total duration the same as a breve,
///    regardless of how the breve was divided up.
//---------------------------------------------------------

TEST_F(Engraving_NoteTests, LongNoteAfterShort_183746)
{
    Score* score = ScoreRW::readScore(NOTE_DATA_DIR + "empty.mscx");
    score->doLayout();

    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_128TH);
    score->inputState().setNoteEntryMode(true);

    score->cmdEnterRest(DurationType::V_128TH);

    score->inputState().setDuration(DurationType::V_BREVE);
    score->cmdAddPitch(47, 0, 0);

    Segment* s = score->tick2segment(TDuration(DurationType::V_128TH).ticks());
    EXPECT_TRUE(s && s->segmentType() == SegmentType::ChordRest);
    EXPECT_TRUE(s->tick() == Fraction(1, 128));

    EngravingItem* e = s->firstElementForNavigation(0);
    EXPECT_TRUE(e && e->isNote());

    std::vector<Note*> nl = toNote(e)->tiedNotes();
    EXPECT_TRUE(nl.size() >= 3);   // the breve must be divided across at least 3 measures
    Fraction totalTicks = Fraction(0, 1);
    for (Note* n : nl) {
        totalTicks += n->chord()->durationTypeTicks();
    }
    Fraction breveTicks = TDuration(DurationType::V_BREVE).ticks();
    EXPECT_TRUE(totalTicks == breveTicks);   // total duration same as a breve
}

TEST_F(Engraving_NoteTests, PianomaniaHeldNoteTogglesCompleteTieChain)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + "empty.mscx");
    score->doLayout();

    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_128TH);
    score->inputState().setNoteEntryMode(true);
    score->cmdEnterRest(DurationType::V_128TH);
    score->inputState().setDuration(DurationType::V_BREVE);
    score->cmdAddPitch(47, 0, 0);

    Segment* segment = score->tick2segment(TDuration(DurationType::V_128TH).ticks());
    ASSERT_TRUE(segment);
    EngravingItem* firstElement = segment->firstElementForNavigation(0);
    ASSERT_TRUE(firstElement && firstElement->isNote());
    std::vector<Note*> tieChain = toNote(firstElement)->tiedNotes();
    ASSERT_GE(tieChain.size(), 3u);

    score->startCmd(TranslatableString::untranslatable("Toggle held tie chain"));
    score->togglePianomaniaHeldNotes({ tieChain[1] });
    score->endCmd();
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNote();
    }));

    score->undoRedo(true, nullptr);
    EXPECT_TRUE(std::none_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNote();
    }));

    score->undoRedo(false, nullptr);
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNote();
    }));
    score->undoRedo(true, nullptr);

    tieChain.front()->setPianomaniaHeldNote(true);
    score->startCmd(TranslatableString::untranslatable("Repair mixed held tie chain"));
    score->togglePianomaniaHeldNotes({ tieChain.back() });
    score->endCmd();
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNote();
    }));

    score->startCmd(TranslatableString::untranslatable("Toggle selected members of one tie chain"));
    score->togglePianomaniaHeldNotes({ tieChain.front(), tieChain.back() });
    score->endCmd();
    EXPECT_TRUE(std::none_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNote();
    }));
}

TEST_F(Engraving_NoteTests, PianomaniaHeldNotePulseUpdatesCompleteSelectedTieChains)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + "empty.mscx");
    score->doLayout();

    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_128TH);
    score->inputState().setNoteEntryMode(true);
    score->cmdEnterRest(DurationType::V_128TH);
    score->inputState().setDuration(DurationType::V_BREVE);
    score->cmdAddPitch(47, 0, 0);

    Segment* segment = score->tick2segment(TDuration(DurationType::V_128TH).ticks());
    ASSERT_TRUE(segment);
    EngravingItem* firstElement = segment->firstElementForNavigation(0);
    ASSERT_TRUE(firstElement && firstElement->isNote());
    std::vector<Note*> firstChain = toNote(firstElement)->tiedNotes();
    ASSERT_GE(firstChain.size(), 3u);

    score->startCmd(TranslatableString::untranslatable("Add second held pulse tie chain"));
    score->addInterval(8, { firstChain.front() });
    score->endCmd();
    ASSERT_GE(firstChain.front()->chord()->notes().size(), 2u);
    Note* secondChainStart = firstChain.front()->chord()->upNote();
    ASSERT_TRUE(secondChainStart);
    ASSERT_NE(secondChainStart, firstChain.front());
    std::vector<Note*> secondChain = secondChainStart->tiedNotes();
    ASSERT_EQ(secondChain.size(), firstChain.size());

    for (Note* note : firstChain) {
        note->setPianomaniaHeldNote(true);
        note->setPianomaniaHeldNotePulse(Note::PianomaniaHeldNotePulse::Quarter);
    }
    for (Note* note : secondChain) {
        note->setPianomaniaHeldNote(true);
        note->setPianomaniaHeldNotePulse(Note::PianomaniaHeldNotePulse::SixtyFourth);
    }

    score->startCmd(TranslatableString::untranslatable("Set mixed held pulse selection"));
    score->setPianomaniaHeldNotePulse({ firstChain[1], secondChain.back(), firstChain.back() },
                                      static_cast<int>(Note::PianomaniaHeldNotePulse::Sixteenth));
    score->endCmd();
    const auto hasSixteenthPulse = [](const Note* note) {
        return note->pianomaniaHeldNotePulse() == Note::PianomaniaHeldNotePulse::Sixteenth;
    };
    EXPECT_TRUE(std::all_of(firstChain.cbegin(), firstChain.cend(), hasSixteenthPulse));
    EXPECT_TRUE(std::all_of(secondChain.cbegin(), secondChain.cend(), hasSixteenthPulse));

    score->undoRedo(true, nullptr);
    EXPECT_TRUE(std::all_of(firstChain.cbegin(), firstChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNotePulse() == Note::PianomaniaHeldNotePulse::Quarter;
    }));
    EXPECT_TRUE(std::all_of(secondChain.cbegin(), secondChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNotePulse() == Note::PianomaniaHeldNotePulse::SixtyFourth;
    }));
    score->undoRedo(false, nullptr);

    score->startCmd(TranslatableString::untranslatable("Set triplet held pulse"));
    score->setPianomaniaHeldNotePulseTriplet({ firstChain.front(), secondChain.front() }, true);
    score->endCmd();
    EXPECT_TRUE(std::all_of(firstChain.cbegin(), firstChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNotePulseTriplet();
    }));
    EXPECT_TRUE(std::all_of(secondChain.cbegin(), secondChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNotePulseTriplet();
    }));

    score->startCmd(TranslatableString::untranslatable("Clear first held pulse"));
    score->setPianomaniaHeldNotePulse({ firstChain[1] }, static_cast<int>(Note::PianomaniaHeldNotePulse::None));
    score->endCmd();
    EXPECT_TRUE(std::all_of(firstChain.cbegin(), firstChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNotePulse() == Note::PianomaniaHeldNotePulse::None
               && !note->pianomaniaHeldNotePulseTriplet();
    }));

    score->startCmd(TranslatableString::untranslatable("Disable second held tie chain"));
    score->togglePianomaniaHeldNotes({ secondChain.front() });
    score->endCmd();
    EXPECT_TRUE(std::all_of(secondChain.cbegin(), secondChain.cend(), [](const Note* note) {
        return !note->pianomaniaHeldNote()
               && note->pianomaniaHeldNotePulse() == Note::PianomaniaHeldNotePulse::None
               && !note->pianomaniaHeldNotePulseTriplet();
    }));

    score->undoRedo(true, nullptr);
    EXPECT_TRUE(std::all_of(secondChain.cbegin(), secondChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNote()
               && note->pianomaniaHeldNotePulse() == Note::PianomaniaHeldNotePulse::Sixteenth
               && note->pianomaniaHeldNotePulseTriplet();
    }));
    score->undoRedo(false, nullptr);
    EXPECT_TRUE(std::all_of(secondChain.cbegin(), secondChain.cend(), [](const Note* note) {
        return !note->pianomaniaHeldNote()
               && note->pianomaniaHeldNotePulse() == Note::PianomaniaHeldNotePulse::None
               && !note->pianomaniaHeldNotePulseTriplet();
    }));
}

TEST_F(Engraving_NoteTests, PianomaniaHeldNotePitchCurveUpdatesCompleteTieChainWithUndo)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + "empty.mscx");
    score->doLayout();
    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_128TH);
    score->inputState().setNoteEntryMode(true);
    score->cmdEnterRest(DurationType::V_128TH);
    score->inputState().setDuration(DurationType::V_BREVE);
    score->cmdAddPitch(47, 0, 0);

    Segment* segment = score->tick2segment(TDuration(DurationType::V_128TH).ticks());
    ASSERT_TRUE(segment);
    std::vector<Note*> tieChain = toNote(segment->firstElementForNavigation(0))->tiedNotes();
    ASSERT_GE(tieChain.size(), 3u);
    for (Note* note : tieChain) {
        note->setPianomaniaHeldNote(true);
    }
    const int durationTicks = tieChain.front()->pianomaniaHeldNoteDurationTicks();
    ASSERT_GT(durationTicks, 0);
    const PianomaniaHeldNotePitchCurve curve {
        { 0, 0, 0 }, { durationTicks / 4, -200, -1200 }, { durationTicks, -1200, 0 }
    };

    EXPECT_FALSE(score->setPianomaniaHeldNotePitchCurve(
        { tieChain[1] }, { { 0, 0, 0 }, { durationTicks - 1, -1200, 0 } }));

    score->startCmd(TranslatableString::untranslatable("Set Held Note pitch curve"));
    EXPECT_TRUE(score->setPianomaniaHeldNotePitchCurve({ tieChain[1] }, curve));
    score->endCmd();
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [&curve](const Note* note) {
        return note->pianomaniaHeldNotePitchCurve() == curve;
    }));

    score->undoRedo(true, nullptr);
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNotePitchCurve().empty();
    }));
    score->undoRedo(false, nullptr);
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [&curve](const Note* note) {
        return note->pianomaniaHeldNotePitchCurve() == curve;
    }));

    score->startCmd(TranslatableString::untranslatable("Clear Held Note pitch curve"));
    EXPECT_TRUE(score->setPianomaniaHeldNotePitchCurve({ tieChain.back() }, {}));
    score->endCmd();
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaHeldNotePitchCurve().empty();
    }));
}

TEST_F(Engraving_NoteTests, PianomaniaShakeNoteTogglesCompleteTieChain)
{
    MasterScore* score = ScoreRW::readScore(NOTE_DATA_DIR + "empty.mscx");
    score->doLayout();

    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_128TH);
    score->inputState().setNoteEntryMode(true);
    score->cmdEnterRest(DurationType::V_128TH);
    score->inputState().setDuration(DurationType::V_BREVE);
    score->cmdAddPitch(47, 0, 0);

    Segment* segment = score->tick2segment(TDuration(DurationType::V_128TH).ticks());
    ASSERT_TRUE(segment);
    EngravingItem* firstElement = segment->firstElementForNavigation(0);
    ASSERT_TRUE(firstElement && firstElement->isNote());
    std::vector<Note*> tieChain = toNote(firstElement)->tiedNotes();
    ASSERT_GE(tieChain.size(), 3u);

    score->startCmd(TranslatableString::untranslatable("Toggle shake tie chain"));
    score->togglePianomaniaShakeNotes({ tieChain[1] });
    score->endCmd();
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaShakeNote();
    }));

    score->undoRedo(true, nullptr);
    EXPECT_TRUE(std::none_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaShakeNote();
    }));

    score->undoRedo(false, nullptr);
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaShakeNote();
    }));
    score->undoRedo(true, nullptr);

    tieChain.front()->setPianomaniaShakeNote(true);
    score->startCmd(TranslatableString::untranslatable("Repair mixed shake tie chain"));
    score->togglePianomaniaShakeNotes({ tieChain.back() });
    score->endCmd();
    EXPECT_TRUE(std::all_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaShakeNote();
    }));

    score->startCmd(TranslatableString::untranslatable("Toggle selected members of one shake tie chain"));
    score->togglePianomaniaShakeNotes({ tieChain.front(), tieChain.back() });
    score->endCmd();
    EXPECT_TRUE(std::none_of(tieChain.cbegin(), tieChain.cend(), [](const Note* note) {
        return note->pianomaniaShakeNote();
    }));
}

TEST_F(Engraving_NoteTests, PianomaniaHandFollowsDestinationStaffForPasteAndAddedOctave)
{
    MasterScore* score = ScoreRW::readScore(u"all_elements_data/cross_staff_arp.mscx");
    ASSERT_TRUE(score);

    Chord* rightHandChord = score->firstMeasure()->findChord(Fraction(0, 1), 0);
    Chord* leftHandChord = score->firstMeasure()->findChord(Fraction(1, 2), 4);
    ASSERT_TRUE(rightHandChord);
    ASSERT_TRUE(leftHandChord);

    Note* rightHandNote = rightHandChord->upNote();
    Note* leftHandNote = leftHandChord->upNote();
    ASSERT_TRUE(rightHandNote);
    ASSERT_TRUE(leftHandNote);
    EXPECT_EQ(rightHandNote->pianomaniaHand(), Note::PianomaniaHand::Right);
    EXPECT_EQ(leftHandNote->pianomaniaHand(), Note::PianomaniaHand::Left);

    rightHandNote->setPianomaniaHand(Note::PianomaniaHand::Left);
    rightHandNote->setupAfterRead(rightHandNote->tick(), true);
    EXPECT_EQ(rightHandNote->pianomaniaHand(), Note::PianomaniaHand::Right);

    const size_t rightHandNoteCount = rightHandChord->notes().size();
    const size_t leftHandNoteCount = leftHandChord->notes().size();
    score->startCmd(TranslatableString::untranslatable("Add Pianomania octave"));
    score->addInterval(8, { rightHandNote, leftHandNote });
    score->endCmd();

    ASSERT_EQ(rightHandChord->notes().size(), rightHandNoteCount + 1);
    ASSERT_EQ(leftHandChord->notes().size(), leftHandNoteCount + 1);
    EXPECT_EQ(rightHandChord->upNote()->pianomaniaHand(), Note::PianomaniaHand::Right);
    EXPECT_EQ(leftHandChord->upNote()->pianomaniaHand(), Note::PianomaniaHand::Left);
}
