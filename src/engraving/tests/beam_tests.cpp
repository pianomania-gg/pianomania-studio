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

#include "engraving/dom/beam.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/note.h"
#include "engraving/dom/tremolotwochord.h"

#include "utils/scorerw.h"
#include "utils/scorecomp.h"

using namespace mu::engraving;

static const String BEAM_DATA_DIR("beam_data/");

//---------------------------------------------------------
//   TestBeam
//---------------------------------------------------------

class Engraving_BeamTests : public ::testing::Test
{
public:
    void beam(const char* path);
    static std::vector<Beam*> beamsInScore(MasterScore* score);
};

std::vector<Beam*> Engraving_BeamTests::beamsInScore(MasterScore* score)
{
    std::vector<Beam*> beams;
    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(SegmentType::ChordRest); segment;
             segment = segment->next(SegmentType::ChordRest)) {
            EngravingItem* item = segment->element(0);
            if (!item || !item->isChordRest()) {
                continue;
            }
            Beam* beam = toChordRest(item)->beam();
            if (beam && (beams.empty() || beams.back() != beam)) {
                beams.push_back(beam);
            }
        }
    }
    return beams;
}

//---------------------------------------------------------
//   beam
//---------------------------------------------------------
void Engraving_BeamTests::beam(const char* path)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + String::fromUtf8(path));
    EXPECT_TRUE(score);
    EXPECT_TRUE(ScoreComp::saveCompareScore(score, String::fromUtf8(path), BEAM_DATA_DIR + String::fromUtf8(path)));
    delete score;
}

TEST_F(Engraving_BeamTests, beamA)
{
    beam("Beam-A.mscx");
}

TEST_F(Engraving_BeamTests, beamB)
{
    beam("Beam-B.mscx");
}

TEST_F(Engraving_BeamTests, beamC)
{
    beam("Beam-C.mscx");
}

TEST_F(Engraving_BeamTests, beamD)
{
    beam("Beam-D.mscx");
}

TEST_F(Engraving_BeamTests, beamE)
{
    beam("Beam-E.mscx");
}

TEST_F(Engraving_BeamTests, beamF)
{
    beam("Beam-F.mscx");
}

TEST_F(Engraving_BeamTests, beamG)
{
    beam("Beam-G.mscx");
}

// make sure the beam end positions are correct for 2+ beams
TEST_F(Engraving_BeamTests, beamPositions)
{
    beam("beamPositions.mscx");
}

TEST_F(Engraving_BeamTests, pianomaniaAutomaticBeamPlacement)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + u"beamPositions.mscx");
    ASSERT_TRUE(score);
    score->doLayout();

    const std::vector<Beam*> beams = beamsInScore(score);
    ASSERT_EQ(beams.size(), 76u);
    auto eighthSpaces = [](Beam* beam) {
        return PairF(std::round(beam->beamPos().first * 8.0), std::round(beam->beamPos().second * 8.0));
    };

    // One-beam first-ledger cases leave a staff line between the beam and ledger, mirrored by stem direction.
    EXPECT_EQ(eighthSpaces(beams[8]), PairF(-4.0, -4.0));
    EXPECT_EQ(eighthSpaces(beams[9]), PairF(40.0, 40.0));
    // Line-note semiquavers use the approved short 3.25sp total stem placement in both directions.
    EXPECT_EQ(eighthSpaces(beams[23]), PairF(8.0, 8.0));
    EXPECT_EQ(eighthSpaces(beams[31]), PairF(24.0, 24.0));
    // Line-note demisemiquavers use 4sp total stems with hanging-up and sitting-down outer beams.
    EXPECT_EQ(eighthSpaces(beams[42]), PairF(2.0, 2.0));
    EXPECT_EQ(eighthSpaces(beams[50]), PairF(30.0, 30.0));

    delete score;
}

TEST_F(Engraving_BeamTests, pianomaniaManualBeamPlacementIsPreserved)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + u"beamPositions.mscx");
    ASSERT_TRUE(score);
    score->doLayout();

    const std::vector<Beam*> beams = beamsInScore(score);
    ASSERT_FALSE(beams.empty());
    beams.front()->setBeamPos(PairF(1.25, 1.75));
    score->doLayout();

    EXPECT_TRUE(beams.front()->userModified());
    EXPECT_NEAR(beams.front()->beamPos().first, 1.25, 0.0001);
    EXPECT_NEAR(beams.front()->beamPos().second, 1.75, 0.0001);

    delete score;
}

TEST_F(Engraving_BeamTests, pianomaniaAutomaticBeamRiseCaps)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + u"flippedDirection.mscx");
    ASSERT_TRUE(score);
    score->doLayout();

    bool foundNonFlatBeam = false;
    for (Beam* beam : beamsInScore(score)) {
        bool eligible = !beam->userModified() && !beam->cross() && !beam->isGrace()
                        && std::abs(beam->beamSpacing() - 3) < 0.0001;
        const staff_idx_t staffIdx = beam->elements().front()->vStaffIdx();
        for (ChordRest* cr : beam->elements()) {
            eligible = eligible && cr->isChord() && cr->vStaffIdx() == staffIdx && cr->up() == beam->up()
                       && cr->beams() >= 1 && cr->beams() <= 3;
        }
        if (!eligible) {
            continue;
        }

        const double width = std::abs(beam->endAnchor().x() - beam->startAnchor().x()) / beam->spatium();
        const double maximumRise = width < 3.0 ? 0.5 : width < 8.0 ? 1.0 : 2.0;
        const PairF position = beam->beamPos();
        const double rise = std::abs(position.second - position.first);
        EXPECT_LE(rise, maximumRise + 0.0001);
        foundNonFlatBeam |= rise > 0.0001;
    }
    EXPECT_TRUE(foundNonFlatBeam);

    delete score;
}

TEST_F(Engraving_BeamTests, pianomaniaRisingGroupsSlantWithTheMelody)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + u"pianomaniaRisingGroups.mscx");
    ASSERT_TRUE(score);
    score->doLayout();

    // Für Elise bars 3 and 4: C4-E4-A4 and E4-G#4-B4 sixteenths, stems up, with the beam inside the staff.
    const std::vector<Beam*> beams = beamsInScore(score);
    ASSERT_EQ(beams.size(), 2u);
    for (Beam* beam : beams) {
        ASSERT_TRUE(beam->up());
        // Positions grow downward, so a beam that rises with the melody ends at least a quarter space higher.
        const PairF position = beam->beamPos();
        EXPECT_LE(position.second, position.first - 0.25 + 0.0001);
    }

    delete score;
}

// if the beamNoSlope style parameter is true, all beams are flat
TEST_F(Engraving_BeamTests, beamNoSlope)
{
    beam("beamNoSlope.mscx");
}

TEST_F(Engraving_BeamTests, flippedDirection)
{
    beam("flippedDirection.mscx");
}

TEST_F(Engraving_BeamTests, wideBeams)
{
    beam("wideBeams.mscx");
}

TEST_F(Engraving_BeamTests, flatBeams)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + u"flatBeams.mscx");
    EXPECT_TRUE(score);
    EXPECT_TRUE(ScoreComp::saveCompareScore(score, u"flatBeams.mscx", BEAM_DATA_DIR + u"flatBeams-ref.mscx"));
    delete score;
}

// cross staff beaming is not yet supported
// in the new beams code
TEST_F(Engraving_BeamTests, DISABLED_beamCrossMeasure2)
{
    beam("Beam-CrossM2.mscx");
}

TEST_F(Engraving_BeamTests, DISABLED_beamCrossMeasure3)
{
    beam("Beam-CrossM3.mscx");
}

TEST_F(Engraving_BeamTests, DISABLED_beamCrossMeasure4)
{
    beam("Beam-CrossM4.mscx");
}

//---------------------------------------------------------
//   beamCrossMeasure1
//   This method simulates following operations:
//   - Update the score
//   - Check if the beam has been recreated. If yes, this is wrong behaviour
//---------------------------------------------------------

// cross measure beams are not yet supported
// in the refactored beams code
TEST_F(Engraving_BeamTests, DISABLED_beamCrossMeasure1)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + u"Beam-CrossM1.mscx");
    EXPECT_TRUE(score);

    Measure* first_measure = score->firstMeasure();
    EXPECT_TRUE(first_measure);

    // find the first segment that has a chord
    Segment* s = first_measure->first(SegmentType::ChordRest);
    while (s && !s->element(0)->isChord()) {
        s = s->next(SegmentType::ChordRest);
    }
    EXPECT_TRUE(s);

    // locate the first beam
    ChordRest* first_note = toChordRest(s->element(0));
    EXPECT_TRUE(first_note);

    Beam* b = first_note->beam();
    score->update();
    // locate the beam again, and check if it is still b
    Beam* new_b = first_note->beam();

    EXPECT_EQ(new_b, b);

    delete score;
}

//---------------------------------------------------------
//   beamStemDir
//   This method tests if a beam's stem direction will be set to
//   all its chords and will not affect other beams in score
//---------------------------------------------------------

TEST_F(Engraving_BeamTests, beamStemDir)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + u"beamStemDir.mscx");
    EXPECT_TRUE(score);

    Measure* m1 = score->firstMeasure();
    ChordRest* cr = toChordRest(m1->findSegment(SegmentType::ChordRest, m1->tick())->element(0));

    cr->beam()->setDirection(DirectionV::UP);

    score->update();
    score->doLayout();

    EXPECT_TRUE(ScoreComp::saveCompareScore(score, u"beamStemDir-01.mscx", BEAM_DATA_DIR + u"beamStemDir-01-ref.mscx"));

    delete score;
}

//---------------------------------------------------------
//   flipBeamStemDir
//   This method tests if a beam's stem direction will be set to
//   all its chords and will not affect other beams in score
//   after using the flip command
//---------------------------------------------------------

TEST_F(Engraving_BeamTests, flipBeamStemDir)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + u"flipBeamStemDir.mscx");
    EXPECT_TRUE(score);

    Measure* m1 = score->firstMeasure();
    ChordRest* cr = toChordRest(m1->findSegment(SegmentType::ChordRest, m1->tick())->element(0));
    Chord* c2 = toChord(cr->beam()->elements()[1]);

    score->select(c2);
    score->startCmd(TranslatableString::untranslatable("Engraving beam tests"));
    score->cmdFlip();
    score->endCmd();
    cr->beam()->setDirection(DirectionV::DOWN);

    score->update();
    score->doLayout();

    EXPECT_TRUE(ScoreComp::saveCompareScore(score, u"flipBeamStemDir-01.mscx", BEAM_DATA_DIR + u"flipBeamStemDir-01-ref.mscx"));

    delete score;
}

//---------------------------------------------------------
//   flipTremoloStemDir
//   This method tests if a tremolo's stem direction will be set to
//   all its chords and will not affect other tremolos in score
//   after using the flip command
//---------------------------------------------------------

TEST_F(Engraving_BeamTests, flipTremoloStemDir)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + "flipTremoloStemDir.mscx");
    EXPECT_TRUE(score);

    Measure* m1 = score->firstMeasure();
    ChordRest* cr = toChordRest(m1->findSegment(SegmentType::ChordRest, m1->tick())->element(0));
    TremoloTwoChord* t = toChord(cr)->tremoloTwoChord();
    Chord* c1 = t->chord1();
    Chord* c2 = t->chord2();
    EXPECT_TRUE(t->up() && c1->up() && c2->up());

    score->select(c1->upNote());
    score->startCmd(TranslatableString::untranslatable("Engraving beam tests"));
    score->cmdFlip();
    score->endCmd();

    score->update();
    score->doLayout();
    EXPECT_FALSE(t->up() || c1->up() || c2->up());

    delete score;
}

TEST_F(Engraving_BeamTests, deleteBeamStemDirection)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + "deleteBeamStemDirection.mscx");
    EXPECT_TRUE(score);

    Measure* m1 = score->firstMeasure();
    ChordRest* cr1 = toChordRest(m1->findSegment(SegmentType::ChordRest, Fraction(0, 8))->element(0));
    EXPECT_TRUE(cr1);
    ChordRest* cr2 = toChordRest(m1->findSegment(SegmentType::ChordRest, Fraction(1, 8))->element(0));
    EXPECT_TRUE(cr2);
    ChordRest* cr3 = toChordRest(m1->findSegment(SegmentType::ChordRest, Fraction(2, 8))->element(0));
    EXPECT_TRUE(cr3);
    ChordRest* cr4 = toChordRest(m1->findSegment(SegmentType::ChordRest, Fraction(3, 8))->element(0));
    EXPECT_TRUE(cr4);

    for (ChordRest* cr : { cr1, cr2, cr3, cr4 }) {
        EXPECT_TRUE(cr->ldata()->up);
    }

    score->startCmd(TranslatableString::untranslatable("Engraving beam tests"));
    score->select({ cr2, cr3, cr4 }, SelectType::RANGE);
    score->cmdDeleteSelection();
    score->endCmd();
    score->setLayoutAll();
    score->doLayout();

    EXPECT_FALSE(cr1->ldata()->up);

    score->undoRedo(true, nullptr);

    toChord(cr1)->setStemDirection(DirectionV::UP);

    score->startCmd(TranslatableString::untranslatable("Engraving beam tests"));
    score->select({ cr2, cr3, cr4 }, SelectType::RANGE);
    score->cmdDeleteSelection();
    score->endCmd();
    score->setLayoutAll();
    score->doLayout();

    EXPECT_TRUE(cr1->ldata()->up);
}

TEST_F(Engraving_BeamTests, drumKitBeam)
{
    MasterScore* score = ScoreRW::readScore(BEAM_DATA_DIR + "drumKitBeam.mscx");
    EXPECT_TRUE(score);
    score->setLayoutAll();
    score->doLayout();
    Measure* m = score->firstMeasure();
    Chord* cr1 = toChord(m->findSegment(SegmentType::ChordRest, Fraction(0, 1))->element(0));
    EXPECT_TRUE(cr1);
    EXPECT_TRUE(cr1->up() && cr1->stemDirection() == DirectionV::UP);
    Chord* cr2 = toChord(m->findSegment(SegmentType::ChordRest, Fraction(2, 8))->element(0));
    EXPECT_TRUE(cr2);
    EXPECT_TRUE(cr2->up() && cr2->stemDirection() == DirectionV::UP);
    Chord* cr3 = toChord(m->findSegment(SegmentType::ChordRest, Fraction(3, 8))->element(0));
    EXPECT_TRUE(cr3);
    EXPECT_TRUE(cr3->up() && cr3->stemDirection() == DirectionV::UP);

    score->startCmd(TranslatableString::untranslatable("Engraving beam tests"));
    score->select({ cr1, cr2, cr3 }, SelectType::RANGE);
    score->cmdFlip();
    score->setLayoutAll();
    score->doLayout();
    score->endCmd();

    EXPECT_TRUE(!cr1->up() && cr1->stemDirection() == DirectionV::DOWN);
    EXPECT_TRUE(!cr2->up() && cr2->stemDirection() == DirectionV::DOWN);
    EXPECT_TRUE(!cr3->up() && cr3->stemDirection() == DirectionV::DOWN);

    score->undoRedo(true, nullptr);

    EXPECT_TRUE(cr1->up() && cr1->stemDirection() == DirectionV::UP);
    // These chords inherit their direction from the beam
    EXPECT_TRUE(cr2->up() && cr2->stemDirection() == DirectionV::AUTO);
    EXPECT_TRUE(cr3->up() && cr3->stemDirection() == DirectionV::AUTO);
}
