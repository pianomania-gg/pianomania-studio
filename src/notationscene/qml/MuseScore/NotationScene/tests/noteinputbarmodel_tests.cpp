/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "engraving/dom/chord.h"
#include "engraving/dom/durationtype.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/tie.h"
#include "engraving/tests/utils/scorerw.h"
#include "notationscene/qml/MuseScore/NotationScene/noteinputbarmodel.h"
#include "types/translatablestring.h"

using namespace mu::engraving;
using namespace mu::notation;

namespace {
std::vector<Note*> createHeldTieChain(MasterScore* score)
{
    score->doLayout();
    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_128TH);
    score->inputState().setNoteEntryMode(true);
    score->cmdEnterRest(DurationType::V_128TH);
    score->inputState().setDuration(DurationType::V_BREVE);
    score->cmdAddPitch(47, 0, 0);

    Segment* segment = score->tick2segment(TDuration(DurationType::V_128TH).ticks());
    EngravingItem* firstElement = segment ? segment->firstElementForNavigation(0) : nullptr;
    if (!firstElement || !firstElement->isNote()) {
        return {};
    }
    std::vector<Note*> chain = toNote(firstElement)->tiedNotes();
    for (Note* note : chain) {
        note->setPianomaniaHeldNote(true);
    }
    return chain;
}

std::vector<Note*> createThreeWholeHeldTieChain(MasterScore* score)
{
    score->doLayout();
    score->inputState().setTrack(0);
    score->inputState().setSegment(score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest));
    score->inputState().setDuration(DurationType::V_BREVE);
    score->inputState().setDots(1);
    score->inputState().setNoteEntryMode(true);
    score->cmdAddPitch(47, 0, 0);

    Segment* segment = score->tick2segment(Fraction(0, 1), false, SegmentType::ChordRest);
    EngravingItem* firstElement = segment ? segment->firstElementForNavigation(0) : nullptr;
    if (!firstElement || !firstElement->isNote()) {
        return {};
    }
    std::vector<Note*> chain = toNote(firstElement)->tiedNotes();
    for (Note* note : chain) {
        note->setPianomaniaHeldNote(true);
    }
    return chain;
}

void expectTimingMarker(const QVariant& value, int scoreTick, bool writtenNoteBoundary)
{
    const QVariantMap marker = value.toMap();
    EXPECT_EQ(marker.value("scoreTick").toInt(), scoreTick);
    EXPECT_EQ(marker.value("writtenNoteBoundary").toBool(), writtenNoteBoundary);
}
}

TEST(NoteInputBarModelTests, ResolvesHeldChainFromContinuationAndTie)
{
    MasterScore* score = ScoreRW::readScore(u"../../../../../engraving/tests/note_data/empty.mscx");
    ASSERT_TRUE(score);
    std::vector<Note*> chain = createHeldTieChain(score);
    ASSERT_GE(chain.size(), 3u);

    EXPECT_EQ(NoteInputBarModel::resolveHeldNoteSettingsChain({ chain[1] }, {}), chain);
    ASSERT_TRUE(chain.front()->tieFor());
    EXPECT_EQ(NoteInputBarModel::resolveHeldNoteSettingsChain({}, { chain.front()->tieFor() }), chain);

    chain.back()->setPianomaniaHeldNote(false);
    EXPECT_TRUE(NoteInputBarModel::resolveHeldNoteSettingsChain({ chain.front() }, {}).empty());
    delete score;
}

TEST(NoteInputBarModelTests, ResolvesMultipleHeldNoteTargetsForSharedPulseEditing)
{
    MasterScore* score = ScoreRW::readScore(u"../../../../../engraving/tests/note_data/empty.mscx");
    ASSERT_TRUE(score);
    std::vector<Note*> firstChain = createHeldTieChain(score);
    ASSERT_GE(firstChain.size(), 3u);

    score->startCmd(TranslatableString::untranslatable("Add second held note"));
    score->addInterval(8, { firstChain.front() });
    score->endCmd();
    Note* secondStart = firstChain.front()->chord()->upNote();
    ASSERT_TRUE(secondStart && secondStart != firstChain.front());
    std::vector<Note*> secondChain = secondStart->tiedNotes();
    ASSERT_EQ(secondChain.size(), firstChain.size());
    for (Note* note : secondChain) {
        note->setPianomaniaHeldNote(true);
    }

    const std::vector<Note*> targets
        = NoteInputBarModel::resolveHeldNoteSettingsTargets({ firstChain[1], secondChain.back() }, {});
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_NE(std::find(targets.cbegin(), targets.cend(), firstChain.front()), targets.cend());
    EXPECT_NE(std::find(targets.cbegin(), targets.cend(), secondChain.front()), targets.cend());

    secondChain.back()->setPianomaniaHeldNote(false);
    EXPECT_TRUE(NoteInputBarModel::resolveHeldNoteSettingsTargets({ firstChain.front(), secondChain.front() }, {}).empty());
    delete score;
}

TEST(NoteInputBarModelTests, WorkingPitchCurveStateEnforcesGraphConstraints)
{
    NoteInputBarModel model(nullptr, 480);
    EXPECT_TRUE(model.heldNoteSettingsValid());
    model.setHeldNotePitchBendEnabled(true);
    EXPECT_FALSE(model.heldNoteSettingsValid());
    model.addHeldNotePitchCurvePoint(120, -200);
    EXPECT_TRUE(model.heldNoteSettingsValid());

    QVariantList points = model.heldNotePitchCurve();
    ASSERT_EQ(points.size(), 3);
    EXPECT_EQ(points[1].toMap().value("scoreTick").toInt(), 120);
    EXPECT_EQ(points[1].toMap().value("pitch").toInt(), -200);

    model.beginHeldNoteSettingsEdit();
    model.setHeldNotePitchCurvePoint(1, 140, -250);
    model.setHeldNotePitchCurvePoint(1, 160, -300);
    model.endHeldNoteSettingsEdit();
    EXPECT_TRUE(model.heldNoteSettingsCanUndo());
    model.undoHeldNoteSettingsEdit();
    points = model.heldNotePitchCurve();
    EXPECT_EQ(points[1].toMap().value("scoreTick").toInt(), 120);
    EXPECT_EQ(points[1].toMap().value("pitch").toInt(), -200);
    EXPECT_TRUE(model.heldNoteSettingsCanRedo());
    model.redoHeldNoteSettingsEdit();
    points = model.heldNotePitchCurve();
    EXPECT_EQ(points[1].toMap().value("scoreTick").toInt(), 160);
    EXPECT_EQ(points[1].toMap().value("pitch").toInt(), -300);

    model.removeHeldNotePitchCurvePoint(0);
    model.removeHeldNotePitchCurvePoint(points.size() - 1);
    EXPECT_EQ(model.heldNotePitchCurve().size(), 3);
    model.removeHeldNotePitchCurvePoint(1);
    EXPECT_EQ(model.heldNotePitchCurve().size(), 2);
    model.undoHeldNoteSettingsEdit();
    EXPECT_EQ(model.heldNotePitchCurve().size(), 3);

    model.setHeldNotePitchCurvePoint(0, 200, 100);
    points = model.heldNotePitchCurve();
    EXPECT_EQ(points[0].toMap().value("scoreTick").toInt(), 0);
    EXPECT_EQ(points[0].toMap().value("pitch").toInt(), 0);
    EXPECT_EQ(points.back().toMap().value("scoreTick").toInt(), 480);

    model.setHeldNotePitchCurvePointSlope(1, 1750);
    points = model.heldNotePitchCurve();
    EXPECT_EQ(points[1].toMap().value("scoreTick").toInt(), 160);
    EXPECT_EQ(points[1].toMap().value("pitch").toInt(), -300);
    EXPECT_EQ(points[1].toMap().value("slope").toInt(), 1750);
    model.setHeldNotePitchCurvePoint(1, 121, -199);
    points = model.heldNotePitchCurve();
    EXPECT_EQ(points[1].toMap().value("slope").toInt(), 1750);

    model.setHeldNotePitchCurvePointSlope(1, 0);
    model.setHeldNotePitchCurvePoint(1, 120, 0);
    model.setHeldNotePitchCurvePoint(points.size() - 1, 200, 0);
    EXPECT_FALSE(model.heldNoteSettingsValid());
    points = model.heldNotePitchCurve();
    EXPECT_EQ(points.back().toMap().value("scoreTick").toInt(), 480);

    model.setHeldNotePitchBendEnabled(false);
    EXPECT_TRUE(model.heldNoteSettingsValid());
}

TEST(NoteInputBarModelTests, TimingGridMarksBeatsAndWrittenNotesAcrossTies)
{
    MasterScore* score = ScoreRW::readScore(u"../../../../../engraving/tests/note_data/empty.mscx");
    ASSERT_TRUE(score);
    std::vector<Note*> chain = createThreeWholeHeldTieChain(score);
    ASSERT_EQ(chain.size(), 3u);

    const std::vector<Note*> resolvedChain = NoteInputBarModel::resolveHeldNoteSettingsChain({ chain[1] }, {});
    ASSERT_EQ(resolvedChain, chain);
    EXPECT_EQ(NoteInputBarModel::heldNoteDurationTicksForChain(resolvedChain), 5760);
    const QVariantList markers = NoteInputBarModel::heldNoteTimingGridForChain(resolvedChain);
    ASSERT_EQ(markers.size(), 49);
    for (int i = 0; i < markers.size(); ++i) {
        expectTimingMarker(markers[i], i * 120, i == 0 || i == 16 || i == 32 || i == 48);
    }
    EXPECT_EQ(markers[0].toMap().value("label").toString(), "1.1");
    EXPECT_TRUE(markers[0].toMap().value("beatBoundary").toBool());
    EXPECT_EQ(markers[4].toMap().value("label").toString(), "1.2");
    EXPECT_EQ(NoteInputBarModel::heldNotePositionLabelForChain(resolvedChain, 600), "1.2 + 120 ticks");

    const QVariantList singleNoteMarkers = NoteInputBarModel::heldNoteTimingGridForChain({ chain.front() });
    EXPECT_EQ(NoteInputBarModel::heldNoteDurationTicksForChain({ chain.front() }), 1920);
    ASSERT_EQ(singleNoteMarkers.size(), 17);
    for (int i = 0; i < singleNoteMarkers.size(); ++i) {
        expectTimingMarker(singleNoteMarkers[i], i * 120, i == 0 || i == 16);
    }

    delete score;
}
