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
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "io/file.h"

#include "engraving/tests/utils/scorerw.h"
#include "engraving/tests/utils/scorecomp.h"

#include "engraving/dom/masterscore.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/expression.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/system.h"

#include "modularity/ioc.h"
#include "importexport/mei/imeiconfiguration.h"
#include "importexport/mei/internal/meireader.h"
#include "importexport/mei/internal/meiwriter.h"
#include "importexport/mei/pmmeiexport.h"

using namespace mu::engraving;

static const String MEI_DIR(u"data/");

////////////////////////////////////////////////////////////////
// Set to true to re-generate the MuseScore reference test files
#define BUILD_MSCORE_REF_FILE false
////////////////////////////////////////////////////////////////

namespace mu::iex::mei {
class Mei_Tests : public ::testing::Test
{
public:
    void meiReadTest(const char* file);

    inline static bool s_generateReferenceFile = BUILD_MSCORE_REF_FILE;
};

void Mei_Tests::meiReadTest(const char* file)
{
    String fileName = String::fromUtf8(file);

    auto importFunc = [](MasterScore* score, const muse::io::path_t& path) -> Err {
        MeiReader meiReader(nullptr);
        return meiReader.import(score, path);
    };

    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    // Load the .mei file
    MasterScore* score = ScoreRW::readScore(MEI_DIR + fileName + u".mei", false, importFunc);
    EXPECT_TRUE(score);

    // Flag to be turned on to generate the test reference .mscx files from the .mei
    if (s_generateReferenceFile) {
        bool res = ScoreRW::saveScore(score, ScoreRW::rootPath() + u"/" + MEI_DIR + fileName + u".mscx");
        EXPECT_TRUE(res);
        return;
    }

    // Compare with the reference MuseScore file
    EXPECT_TRUE(ScoreComp::saveCompareScore(score, fileName + u".mscx", MEI_DIR + fileName + u".mscx"));

    // Save the .mei file for round trip testing
    bool output = ScoreRW::saveScore(score,  fileName + u".test.mei", exportFunc);
    EXPECT_TRUE(output);
    delete score;

    // Compare the mei files
    EXPECT_TRUE(ScoreComp::compareFiles(fileName + u".test.mei", ScoreRW::rootPath() + u"/" + MEI_DIR + fileName + u".mei"));
}

struct PianomaniaPrettifyFlagScope {
    bool previousPrettify = false;
    bool previousForceNormalize = false;

    PianomaniaPrettifyFlagScope(bool prettify, bool forceNormalize)
        : previousPrettify(MScore::pianomaniaPrettifySlursFingerings),
        previousForceNormalize(MScore::pianomaniaForceNormalizeSlursFingerings)
    {
        MScore::pianomaniaPrettifySlursFingerings = prettify;
        MScore::pianomaniaForceNormalizeSlursFingerings = forceNormalize;
        MScore::resetPianomaniaSlurFingeringDiagnostics();
    }

    ~PianomaniaPrettifyFlagScope()
    {
        MScore::pianomaniaPrettifySlursFingerings = previousPrettify;
        MScore::pianomaniaForceNormalizeSlursFingerings = previousForceNormalize;
    }
};

struct FingeringExportData {
    std::string tag;
    std::string text;
    std::optional<std::string> pmxy;
    std::optional<double> yOffset;
};

std::string readTestTextFile(const String& fileName)
{
    muse::io::File file(fileName);
    EXPECT_TRUE(file.open(muse::io::IODevice::ReadOnly));
    if (!file.isOpen()) {
        return std::string();
    }

    const auto data = file.readAll();
    return std::string(reinterpret_cast<const char*>(data.constData()), data.size());
}

std::optional<std::string> xmlAttributeValue(const std::string& tag, const std::string& name)
{
    const std::string needle = name + "=\"";
    size_t start = tag.find(needle);
    if (start == std::string::npos) {
        return std::nullopt;
    }

    start += needle.size();
    const size_t end = tag.find('"', start);
    if (end == std::string::npos) {
        return std::nullopt;
    }

    return tag.substr(start, end - start);
}

std::optional<double> xmlAttributeDouble(const std::string& tag, const std::string& name)
{
    const std::optional<std::string> value = xmlAttributeValue(tag, name);
    if (!value.has_value()) {
        return std::nullopt;
    }

    char* end = nullptr;
    const double parsed = std::strtod(value->c_str(), &end);
    if (end == value->c_str()) {
        return std::nullopt;
    }

    return parsed;
}

std::vector<std::string> collectStartTags(const std::string& xmlText, const std::string& elementName)
{
    std::vector<std::string> tags;
    const std::string needle = "<" + elementName;
    size_t cursor = 0;
    while ((cursor = xmlText.find(needle, cursor)) != std::string::npos) {
        const size_t end = xmlText.find('>', cursor);
        if (end == std::string::npos) {
            break;
        }

        tags.push_back(xmlText.substr(cursor, end - cursor + 1));
        cursor = end + 1;
    }

    return tags;
}

std::vector<FingeringExportData> collectFingeringExportData(const std::string& meiText)
{
    std::vector<FingeringExportData> fingerings;
    size_t cursor = 0;
    while ((cursor = meiText.find("<fing", cursor)) != std::string::npos) {
        const size_t tagEnd = meiText.find('>', cursor);
        if (tagEnd == std::string::npos) {
            break;
        }

        const size_t close = meiText.find("</fing>", tagEnd);
        if (close == std::string::npos) {
            break;
        }

        FingeringExportData data;
        data.tag = meiText.substr(cursor, tagEnd - cursor + 1);
        data.text = meiText.substr(tagEnd + 1, close - tagEnd - 1);
        data.pmxy = xmlAttributeValue(data.tag, "pm:xy");
        data.yOffset = xmlAttributeDouble(data.tag, "yOffset");
        fingerings.push_back(data);
        cursor = close + 7;
    }

    return fingerings;
}

std::vector<FingeringExportData> exportPianomaniaFingeringObstacleFixture(bool prettify, bool forceNormalize,
                                                                         const String& outputName, std::string* outputText,
                                                                         int* normalizedManualFingerings)
{
    PianomaniaPrettifyFlagScope flagScope(prettify, forceNormalize);

    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-fingering-obstacles.mscx", false);
    EXPECT_TRUE(score);
    if (!score) {
        return {};
    }

    score->setLayoutAll();
    score->doLayout();

    const bool output = ScoreRW::saveScore(score, outputName, exportFunc);
    EXPECT_TRUE(output);
    delete score;

    if (normalizedManualFingerings) {
        *normalizedManualFingerings = MScore::pianomaniaNormalizedManualFingerings;
    }

    *outputText = readTestTextFile(outputName);
    return collectFingeringExportData(*outputText);
}

TEST_F(Mei_Tests, mei_accid_01) {
    meiReadTest("accid-01");
}

TEST_F(Mei_Tests, mei_accid_02) {
    meiReadTest("accid-02");
}

TEST_F(Mei_Tests, mei_arpeg_01) {
    meiReadTest("arpeg-01");
}

TEST_F(Mei_Tests, mei_artic_01) {
    meiReadTest("artic-01");
}

TEST_F(Mei_Tests, mei_artic_02) {
    meiReadTest("artic-02");
}

TEST_F(Mei_Tests, mei_beam_01) {
    meiReadTest("beam-01");
}

TEST_F(Mei_Tests, mei_beam_02) {
    meiReadTest("beam-02");
}

TEST_F(Mei_Tests, mei_beam_03) {
    meiReadTest("beam-03");
}

TEST_F(Mei_Tests, mei_export_beam_boundaries_ignore_omitted_hidden_chords) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-visible-beam-boundaries.mscx", false);
    ASSERT_TRUE(score);

    const String outputName = u"pianomania-visible-beam-boundaries.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    EXPECT_EQ(collectStartTags(meiText, "beam").size(), 2u);
    EXPECT_EQ(collectStartTags(meiText, "note").size(), 6u);
}

TEST_F(Mei_Tests, mei_export_pianomania_rubato_zone) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-rubato-zone.mscx", false);
    ASSERT_TRUE(score);

    score->setLayoutAll();
    score->doLayout();

    const String outputName = u"pianomania-rubato-zone.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);

    const size_t lineOpen = meiText.find("<line");
    ASSERT_NE(lineOpen, std::string::npos);
    const size_t lineEnd = meiText.find(">", lineOpen);
    ASSERT_NE(lineEnd, std::string::npos);
    const std::string lineTag = meiText.substr(lineOpen, lineEnd - lineOpen + 1);

    EXPECT_NE(lineTag.find("type=\"pm-rubato-zone\""), std::string::npos) << lineTag;
    EXPECT_NE(lineTag.find("pm:whole-measures=\"true\""), std::string::npos) << lineTag;
    EXPECT_NE(lineTag.find("startid=\"#"), std::string::npos) << lineTag;
    EXPECT_NE(lineTag.find("endid=\"#"), std::string::npos) << lineTag;
    EXPECT_NE(lineTag.find("pm:x1y1x2y2=\""), std::string::npos) << lineTag;
    EXPECT_NE(lineTag.find("pm:segments=\""), std::string::npos) << lineTag;
    // Exactly one zone in the fixture.
    EXPECT_EQ(meiText.find("<line", lineOpen + 1), std::string::npos);

    const std::optional<std::string> endid = xmlAttributeValue(lineTag, "endid");
    ASSERT_TRUE(endid.has_value());
    ASSERT_GT(endid->size(), 1u);
    const size_t firstMeasure = meiText.find("<measure");
    ASSERT_NE(firstMeasure, std::string::npos);
    const size_t secondMeasure = meiText.find("<measure", firstMeasure + 1);
    ASSERT_NE(secondMeasure, std::string::npos);
    const size_t secondMeasureEnd = meiText.find("</measure>", secondMeasure);
    ASSERT_NE(secondMeasureEnd, std::string::npos);
    const size_t endAnchor = meiText.find(
        "xml:id=\"" + endid->substr(1) + "\"", secondMeasure);
    EXPECT_LT(endAnchor, secondMeasureEnd);
    EXPECT_EQ(meiText.find("pname=\"c\" oct=\"6\""), std::string::npos);
}

TEST_F(Mei_Tests, mei_export_pianomania_470_properties_survive_mscx_round_trip) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-rubato-zone.mscx", false);
    ASSERT_TRUE(score);

    const String roundTripName = u"pianomania-470-properties.roundtrip.mscx";
    ASSERT_TRUE(ScoreRW::saveScore(score, roundTripName));
    delete score;

    const std::string roundTripMscx = readTestTextFile(roundTripName);
    EXPECT_NE(roundTripMscx.find("<pianomaniaHeldNotePitchCurve version=\"3\">"), std::string::npos);
    EXPECT_NE(roundTripMscx.find(
        "<point scoreTick=\"120\" pitchCents=\"-200\" slopeCentsPerQuarter=\"-960\"/>"), std::string::npos);

    MasterScore* roundTrippedScore = ScoreRW::readScore(roundTripName, true);
    ASSERT_TRUE(roundTrippedScore);

    size_t heldNoteCount = 0;
    size_t pulsingNoteCount = 0;
    size_t quarterPulseCount = 0;
    size_t tripletPulseCount = 0;
    size_t heldPitchCurveCount = 0;
    size_t shakeNoteCount = 0;
    size_t rightHandNoteCount = 0;
    size_t rightHandRestCount = 0;
    roundTrippedScore->scanElements([&](EngravingItem* item) {
        if (item->isRest()) {
            const Rest* rest = toRest(item);
            rightHandRestCount += rest->pianomaniaHand()
                == static_cast<int>(Note::PianomaniaHand::Right) ? 1 : 0;
            return;
        }
        if (!item->isNote()) {
            return;
        }
        const Note* note = toNote(item);
        heldNoteCount += note->pianomaniaHeldNote() ? 1 : 0;
        pulsingNoteCount += note->pianomaniaHeldNotePulse()
            != Note::PianomaniaHeldNotePulse::None ? 1 : 0;
        quarterPulseCount += note->pianomaniaHeldNotePulse()
            == Note::PianomaniaHeldNotePulse::Quarter ? 1 : 0;
        tripletPulseCount += note->pianomaniaHeldNotePulseTriplet() ? 1 : 0;
        heldPitchCurveCount += note->pianomaniaHeldNotePitchCurve().empty() ? 0 : 1;
        shakeNoteCount += note->pianomaniaShakeNote() ? 1 : 0;
        rightHandNoteCount += note->pianomaniaHand() == Note::PianomaniaHand::Right ? 1 : 0;
    });
    EXPECT_EQ(heldNoteCount, 3u);
    EXPECT_EQ(pulsingNoteCount, 2u);
    EXPECT_EQ(quarterPulseCount, 1u);
    EXPECT_EQ(tripletPulseCount, 1u);
    EXPECT_EQ(heldPitchCurveCount, 1u);
    EXPECT_EQ(shakeNoteCount, 1u);
    EXPECT_EQ(rightHandNoteCount, 1u);
    EXPECT_EQ(rightHandRestCount, 1u);

    const auto spanners = roundTrippedScore->spannerMap().findOverlapping(
        0, roundTrippedScore->endTick().ticks());
    const Spanner* rubatoZone = nullptr;
    for (const auto& interval : spanners) {
        if (interval.value && interval.value->isRubatoZone()) {
            rubatoZone = interval.value;
            break;
        }
    }
    ASSERT_TRUE(rubatoZone);
    EXPECT_EQ(rubatoZone->tick().ticks(), 0);
    EXPECT_EQ(rubatoZone->tick2().ticks(), 3840);
    EXPECT_EQ(rubatoZone->track(), 0);
    EXPECT_EQ(rubatoZone->track2(), 0);

    roundTrippedScore->setLayoutAll();
    roundTrippedScore->doLayout();
    const String outputName = u"pianomania-470-properties.roundtrip.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(roundTrippedScore, outputName, exportFunc));
    delete roundTrippedScore;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> noteTags = collectStartTags(meiText, "note");
    std::vector<std::string> regularNoteTags;
    std::copy_if(noteTags.cbegin(), noteTags.cend(), std::back_inserter(regularNoteTags), [](const std::string& tag) {
        return xmlAttributeValue(tag, "pname").has_value();
    });
    ASSERT_GE(regularNoteTags.size(), 4u);
    EXPECT_EQ(xmlAttributeValue(regularNoteTags[0], "held"), "true");
    EXPECT_EQ(xmlAttributeValue(regularNoteTags[0], "heldPulse"), "quarter");
    EXPECT_EQ(xmlAttributeValue(regularNoteTags[0], "heldPulseTriplet"), "true");
    EXPECT_EQ(xmlAttributeValue(regularNoteTags[0], "pm:heldPitchCurveV3"),
              "0:0:-667;120:-200:-960;480:-1200:-1733");
    EXPECT_FALSE(xmlAttributeValue(regularNoteTags[0], "pm:heldPitchCurveV2").has_value());
    EXPECT_EQ(xmlAttributeValue(regularNoteTags[1], "held"), "true");
    EXPECT_EQ(xmlAttributeValue(regularNoteTags[1], "heldPulse"), "eighth");
    EXPECT_EQ(xmlAttributeValue(regularNoteTags[1], "heldPulseTriplet"), "false");
    EXPECT_EQ(xmlAttributeValue(regularNoteTags[2], "held"), "true");
    EXPECT_FALSE(xmlAttributeValue(regularNoteTags[2], "heldPulse").has_value());
    EXPECT_FALSE(xmlAttributeValue(regularNoteTags[2], "heldPulseTriplet").has_value());
    EXPECT_FALSE(xmlAttributeValue(regularNoteTags[3], "held").has_value());
    EXPECT_FALSE(xmlAttributeValue(regularNoteTags[3], "heldPulse").has_value());
    EXPECT_FALSE(xmlAttributeValue(regularNoteTags[3], "heldPulseTriplet").has_value());
    EXPECT_NE(meiText.find("shake=\"true\""), std::string::npos);
    EXPECT_NE(meiText.find("hand=\"right\""), std::string::npos);
    EXPECT_NE(meiText.find("type=\"pm-rubato-zone\""), std::string::npos);
    const std::vector<std::string> restTags = collectStartTags(meiText, "rest");
    EXPECT_TRUE(std::any_of(restTags.cbegin(), restTags.cend(), [](const std::string& tag) {
        return tag.find("hand=\"right\"") != std::string::npos;
    }));
}

TEST_F(Mei_Tests, mei_export_pianomania_rubato_zone_resolves_hidden_boundaries_to_one_anchor) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(
        MEI_DIR + u"pianomania-rubato-zone-single-anchor.mscx", false);
    ASSERT_TRUE(score);

    score->setLayoutAll();
    score->doLayout();

    const String outputName = u"pianomania-rubato-zone-single-anchor.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> lines = collectStartTags(meiText, "line");
    ASSERT_EQ(lines.size(), 1u);

    const std::optional<std::string> startid = xmlAttributeValue(lines[0], "startid");
    const std::optional<std::string> endid = xmlAttributeValue(lines[0], "endid");
    ASSERT_TRUE(startid.has_value());
    ASSERT_TRUE(endid.has_value());
    EXPECT_EQ(startid, endid);
    ASSERT_GT(startid->size(), 1u);

    const std::string anchorId = "xml:id=\"" + startid->substr(1) + "\"";
    const size_t noteAnchor = meiText.find("<note " + anchorId);
    const size_t chordAnchor = meiText.find("<chord " + anchorId);
    const size_t restAnchor = meiText.find("<rest " + anchorId);
    EXPECT_TRUE(noteAnchor != std::string::npos ||
                chordAnchor != std::string::npos ||
                restAnchor != std::string::npos);
    EXPECT_EQ(meiText.find("pname=\"c\" oct=\"6\""), std::string::npos);
    EXPECT_EQ(meiText.find("pname=\"d\" oct=\"6\""), std::string::npos);
}

TEST_F(Mei_Tests, mei_export_pianomania_rubato_zone_overlap_fails) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-rubato-zone-overlap.mscx", false);
    ASSERT_TRUE(score);

    score->setLayoutAll();
    score->doLayout();

    // Overlapping zones are invalid; the coordinated export must fail.
    EXPECT_FALSE(ScoreRW::saveScore(score, u"pianomania-rubato-zone-overlap.test.mei", exportFunc));
    delete score;
}

TEST_F(Mei_Tests, mei_export_pianomania_pyro_span_free_range) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-pyro-span.mscx", false);
    ASSERT_TRUE(score);

    // The fixture span is free-range: mid-measure start (beat 2 of measure 1)
    // through beat 2 of measure 2, unlike the whole-measure rubato zones.
    const Spanner* pyroSpan = nullptr;
    auto spanners = score->spannerMap().findOverlapping(0, score->endTick().ticks());
    for (auto interval : spanners) {
        if (interval.value && interval.value->isPyroSpan()) {
            pyroSpan = interval.value;
        }
    }
    ASSERT_TRUE(pyroSpan);
    EXPECT_EQ(pyroSpan->tick().ticks(), 480);
    EXPECT_EQ(pyroSpan->tick2().ticks(), 2880);

    score->setLayoutAll();
    score->doLayout();

    const String outputName = u"pianomania-pyro-span.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> lines = collectStartTags(meiText, "line");
    ASSERT_EQ(lines.size(), 1u);
    const std::string& lineTag = lines[0];

    EXPECT_NE(lineTag.find("type=\"pm-pyro-span\""), std::string::npos) << lineTag;
    EXPECT_EQ(lineTag.find("pm:whole-measures"), std::string::npos) << lineTag;
    EXPECT_NE(lineTag.find("pm:x1y1x2y2=\""), std::string::npos) << lineTag;
    EXPECT_NE(lineTag.find("pm:segments=\""), std::string::npos) << lineTag;
    EXPECT_EQ(xmlAttributeValue(lineTag, "pm:start-measure-index"), "0");
    EXPECT_EQ(xmlAttributeValue(lineTag, "pm:start-beat"), "2.0000");
    EXPECT_EQ(xmlAttributeValue(lineTag, "pm:end-measure-index"), "1");
    EXPECT_EQ(xmlAttributeValue(lineTag, "pm:end-beat"), "3.0000");

    const std::optional<std::string> startid = xmlAttributeValue(lineTag, "startid");
    const std::optional<std::string> endid = xmlAttributeValue(lineTag, "endid");
    ASSERT_TRUE(startid.has_value());
    ASSERT_TRUE(endid.has_value());
    ASSERT_GT(startid->size(), 1u);
    ASSERT_GT(endid->size(), 1u);
    EXPECT_NE(startid, endid);

    // The start anchor is the D4 on beat 2 of measure 1.
    const size_t startAnchor = meiText.find("xml:id=\"" + startid->substr(1) + "\"");
    ASSERT_NE(startAnchor, std::string::npos);
    const size_t startAnchorEnd = meiText.find(">", startAnchor);
    const std::string startAnchorTag = meiText.substr(startAnchor, startAnchorEnd - startAnchor);
    EXPECT_NE(startAnchorTag.find("pname=\"d\""), std::string::npos) << startAnchorTag;

    // The reference anchor prefers the E4 attacked at the exact exclusive
    // end tick. The position attributes retain the semantic end independently.
    const size_t endAnchor = meiText.find("xml:id=\"" + endid->substr(1) + "\"");
    ASSERT_NE(endAnchor, std::string::npos);
    const size_t endAnchorEnd = meiText.find(">", endAnchor);
    const std::string endAnchorTag = meiText.substr(endAnchor, endAnchorEnd - endAnchor);
    EXPECT_NE(endAnchorTag.find("pname=\"e\""), std::string::npos) << endAnchorTag;
}

TEST_F(Mei_Tests, mei_export_pianomania_pyro_span_rest_boundary) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-pyro-span-rest-boundary.mscx", false);
    ASSERT_TRUE(score);

    score->setLayoutAll();
    score->doLayout();

    const String outputName = u"pianomania-pyro-span-rest-boundary.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> lines = collectStartTags(meiText, "line");
    ASSERT_EQ(lines.size(), 1u);
    const std::string& lineTag = lines[0];

    EXPECT_EQ(xmlAttributeValue(lineTag, "pm:start-measure-index"), "0");
    EXPECT_EQ(xmlAttributeValue(lineTag, "pm:start-beat"), "2.0000");
    EXPECT_EQ(xmlAttributeValue(lineTag, "pm:end-measure-index"), "0");
    EXPECT_EQ(xmlAttributeValue(lineTag, "pm:end-beat"), "5.0000");

    const std::optional<std::string> startid = xmlAttributeValue(lineTag, "startid");
    const std::optional<std::string> endid = xmlAttributeValue(lineTag, "endid");
    ASSERT_TRUE(startid.has_value());
    ASSERT_TRUE(endid.has_value());
    ASSERT_GT(startid->size(), 1u);
    ASSERT_GT(endid->size(), 1u);

    EXPECT_NE(meiText.find("<rest xml:id=\"" + startid->substr(1) + "\""), std::string::npos);
    EXPECT_NE(meiText.find("xml:id=\"" + endid->substr(1) + "\""), std::string::npos);
}

TEST_F(Mei_Tests, mei_export_pianomania_laser_span_coexists_with_rubato_zone) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-laser-span.mscx", false);
    ASSERT_TRUE(score);

    score->setLayoutAll();
    score->doLayout();

    // The laser span (measure 1) overlaps a rubato zone (measures 1-2);
    // cross-type overlap is valid and the export must succeed.
    const String outputName = u"pianomania-laser-span.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> lines = collectStartTags(meiText, "line");
    ASSERT_EQ(lines.size(), 2u);

    const bool firstIsLaser = lines[0].find("type=\"pm-laser-span\"") != std::string::npos;
    const std::string& laserTag = firstIsLaser ? lines[0] : lines[1];
    const std::string& rubatoTag = firstIsLaser ? lines[1] : lines[0];

    EXPECT_NE(laserTag.find("type=\"pm-laser-span\""), std::string::npos) << laserTag;
    EXPECT_EQ(laserTag.find("pm:whole-measures"), std::string::npos) << laserTag;
    EXPECT_NE(laserTag.find("startid=\"#"), std::string::npos) << laserTag;
    EXPECT_NE(laserTag.find("endid=\"#"), std::string::npos) << laserTag;
    EXPECT_EQ(xmlAttributeValue(laserTag, "pm:start-measure-index"), "0");
    EXPECT_EQ(xmlAttributeValue(laserTag, "pm:start-beat"), "1.0000");
    EXPECT_EQ(xmlAttributeValue(laserTag, "pm:end-measure-index"), "0");
    EXPECT_EQ(xmlAttributeValue(laserTag, "pm:end-beat"), "5.0000");

    EXPECT_NE(rubatoTag.find("type=\"pm-rubato-zone\""), std::string::npos) << rubatoTag;
    EXPECT_NE(rubatoTag.find("pm:whole-measures=\"true\""), std::string::npos) << rubatoTag;
}

TEST_F(Mei_Tests, mei_export_pianomania_pyro_span_overlap_fails) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-pyro-span-overlap.mscx", false);
    ASSERT_TRUE(score);

    score->setLayoutAll();
    score->doLayout();

    // Same-type overlapping spans are invalid; the export must fail.
    EXPECT_FALSE(ScoreRW::saveScore(score, u"pianomania-pyro-span-overlap.test.mei", exportFunc));
    delete score;
}

TEST_F(Mei_Tests, mei_export_beam_and_tuplet_boundaries_ignore_omitted_hidden_chord) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-visible-beam-tuplet-boundaries.mscx", false);
    ASSERT_TRUE(score);

    score->setLayoutAll();
    score->doLayout();

    const String outputName = u"pianomania-visible-beam-tuplet-boundaries.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    EXPECT_EQ(collectStartTags(meiText, "beam").size(), 1u);
    const std::vector<std::string> tupletTags = collectStartTags(meiText, "tuplet");
    ASSERT_EQ(tupletTags.size(), 1u);
    EXPECT_EQ(xmlAttributeValue(tupletTags.front(), "bracket.place"), "above");
    EXPECT_EQ(xmlAttributeValue(tupletTags.front(), "num.place"), "above");
    EXPECT_EQ(collectStartTags(meiText, "note").size(), 2u);

    const size_t beamOpen = meiText.find("<beam");
    const size_t tupletOpen = meiText.find("<tuplet", beamOpen);
    const size_t tupletClose = meiText.find("</tuplet>", tupletOpen);
    const size_t beamClose = meiText.find("</beam>", tupletClose);
    EXPECT_NE(beamOpen, std::string::npos);
    EXPECT_NE(tupletOpen, std::string::npos);
    EXPECT_NE(tupletClose, std::string::npos);
    EXPECT_NE(beamClose, std::string::npos);
}

TEST_F(Mei_Tests, mei_export_tuplets_include_complete_resolved_geometry) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"tuplet-03.mscx", false);
    ASSERT_TRUE(score);
    score->setLayoutAll();
    score->doLayout();

    const String outputName = u"pianomania-tuplet-geometry.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::vector<std::string> tags = collectStartTags(readTestTextFile(outputName), "tuplet");
    ASSERT_GE(tags.size(), 8u);

    bool foundAbove = false;
    bool foundBelow = false;
    bool foundVisibleBracket = false;
    bool foundHiddenBracket = false;
    bool foundNumberOnly = false;
    bool foundHiddenNumberWithBracket = false;
    bool foundRatio = false;
    bool foundHorizontalBracket = false;
    bool foundSlopedBracket = false;

    for (const std::string& tag : tags) {
        EXPECT_EQ(xmlAttributeValue(tag, "pm:tuplet-geometry-version"), "1") << tag;
        const std::optional<std::string> placement = xmlAttributeValue(tag, "pm:tuplet-placement");
        const std::optional<std::string> numberVisible = xmlAttributeValue(tag, "pm:tuplet-number-visible");
        const std::optional<std::string> bracketVisible = xmlAttributeValue(tag, "pm:tuplet-bracket-visible");
        ASSERT_TRUE(placement.has_value()) << tag;
        ASSERT_TRUE(numberVisible.has_value()) << tag;
        ASSERT_TRUE(bracketVisible.has_value()) << tag;

        foundAbove |= placement == "above";
        foundBelow |= placement == "below";
        const bool resolvedNumberVisible = numberVisible == "true";
        const bool resolvedBracketVisible = bracketVisible == "true";
        foundVisibleBracket |= resolvedBracketVisible;
        foundHiddenBracket |= !resolvedBracketVisible;
        foundNumberOnly |= resolvedNumberVisible && !resolvedBracketVisible;
        foundHiddenNumberWithBracket |= !resolvedNumberVisible && resolvedBracketVisible;
        foundRatio |= xmlAttributeValue(tag, "num.format") == "ratio";

        EXPECT_EQ(xmlAttributeValue(tag, "pm:tuplet-number-center").has_value(), resolvedNumberVisible) << tag;
        EXPECT_EQ(xmlAttributeValue(tag, "pm:tuplet-bracket-segments").has_value(), resolvedBracketVisible) << tag;
        EXPECT_EQ(xmlAttributeValue(tag, "pm:tuplet-bracket-hooks").has_value(), resolvedBracketVisible) << tag;

        if (!resolvedBracketVisible) {
            continue;
        }

        const std::string segments = *xmlAttributeValue(tag, "pm:tuplet-bracket-segments");
        const std::string hooks = *xmlAttributeValue(tag, "pm:tuplet-bracket-hooks");
        EXPECT_EQ(std::count(hooks.begin(), hooks.end(), ';'), 1) << tag;
        EXPECT_EQ(std::count(segments.begin(), segments.end(), ';'), resolvedNumberVisible ? 1 : 0) << tag;

        const size_t segmentEnd = segments.find(';');
        std::stringstream stream(segments.substr(0, segmentEnd));
        std::string coordinate;
        std::vector<double> values;
        while (std::getline(stream, coordinate, ',')) {
            values.push_back(std::strtod(coordinate.c_str(), nullptr));
        }
        ASSERT_EQ(values.size(), 4u) << tag;
        ASSERT_GT(std::abs(values[2] - values[0]), 0.000001) << tag;
        const double slope = (values[3] - values[1]) / (values[2] - values[0]);
        foundHorizontalBracket |= std::abs(slope) < 0.001;
        foundSlopedBracket |= std::abs(slope) >= 0.001;
    }

    EXPECT_TRUE(foundAbove);
    EXPECT_TRUE(foundBelow);
    EXPECT_TRUE(foundVisibleBracket);
    EXPECT_TRUE(foundHiddenBracket);
    EXPECT_TRUE(foundNumberOnly);
    EXPECT_TRUE(foundHiddenNumberWithBracket);
    EXPECT_TRUE(foundRatio);
    EXPECT_TRUE(foundHorizontalBracket);
    EXPECT_TRUE(foundSlopedBracket);
}

TEST_F(Mei_Tests, mei_breaks_01) {
    meiReadTest("breaks-01");
}

TEST_F(Mei_Tests, mei_breath_01) {
    meiReadTest("breath-01");
}

TEST_F(Mei_Tests, mei_btrem_01) {
    meiReadTest("btrem-01");
}

TEST_F(Mei_Tests, mei_chord_label_01) {
    meiReadTest("chord-label-01");
}

TEST_F(Mei_Tests, mei_clef_01) {
    meiReadTest("clef-01");
}

TEST_F(Mei_Tests, mei_color_01) {
    meiReadTest("color-01");
}

TEST_F(Mei_Tests, mei_cross_staff_01) {
    meiReadTest("cross-staff-01");
}

TEST_F(Mei_Tests, mei_dir_01) {
    meiReadTest("dir-01");
}

TEST_F(Mei_Tests, mei_dynamic_01) {
    meiReadTest("dynamic-01");
}

TEST_F(Mei_Tests, mei_ending_01) {
    meiReadTest("ending-01");
}

TEST_F(Mei_Tests, mei_fermata_01) {
    meiReadTest("fermata-01");
}

TEST_F(Mei_Tests, mei_fig_bass_01) {
    meiReadTest("fig-bass-01");
}

TEST_F(Mei_Tests, mei_fingering_01) {
    meiReadTest("fingering-01");
}

TEST_F(Mei_Tests, mei_pianomania_fingering_notation_obstacles) {
    std::string baselineText;
    std::string prettifyText;
    std::string forceText;
    int baselineNormalizedManualFingerings = 0;
    int prettifyNormalizedManualFingerings = 0;
    int forceNormalizedManualFingerings = 0;
    const std::vector<FingeringExportData> baselineFingerings = exportPianomaniaFingeringObstacleFixture(
        false, false, u"pianomania-fingering-obstacles.baseline.test.mei", &baselineText, &baselineNormalizedManualFingerings);
    const std::vector<FingeringExportData> prettifyFingerings = exportPianomaniaFingeringObstacleFixture(
        true, false, u"pianomania-fingering-obstacles.prettify.test.mei", &prettifyText, &prettifyNormalizedManualFingerings);
    const std::vector<FingeringExportData> forceFingerings = exportPianomaniaFingeringObstacleFixture(
        true, true, u"pianomania-fingering-obstacles.force.test.mei", &forceText, &forceNormalizedManualFingerings);

    ASSERT_EQ(baselineFingerings.size(), 10u);
    ASSERT_EQ(prettifyFingerings.size(), baselineFingerings.size());
    ASSERT_EQ(forceFingerings.size(), baselineFingerings.size());

    for (const FingeringExportData& fingering : prettifyFingerings) {
        EXPECT_TRUE(fingering.pmxy.has_value());
        EXPECT_TRUE(fingering.yOffset.has_value());
    }

    bool automaticFingeringMoved = false;
    for (size_t i = 0; i + 1 < prettifyFingerings.size(); ++i) {
        ASSERT_TRUE(baselineFingerings[i].yOffset.has_value());
        ASSERT_TRUE(prettifyFingerings[i].yOffset.has_value());
        if (std::abs(*baselineFingerings[i].yOffset - *prettifyFingerings[i].yOffset) > 0.05) {
            automaticFingeringMoved = true;
            break;
        }
    }
    EXPECT_TRUE(automaticFingeringMoved);

    const FingeringExportData& baselineManual = baselineFingerings.back();
    const FingeringExportData& prettifyManual = prettifyFingerings.back();
    const FingeringExportData& forceManual = forceFingerings.back();
    ASSERT_TRUE(baselineManual.yOffset.has_value());
    ASSERT_TRUE(prettifyManual.yOffset.has_value());
    ASSERT_TRUE(forceManual.yOffset.has_value());
    EXPECT_NEAR(*baselineManual.yOffset, *prettifyManual.yOffset, 0.1);
    EXPECT_EQ(baselineNormalizedManualFingerings, 0);
    EXPECT_EQ(prettifyNormalizedManualFingerings, 0);
    EXPECT_GE(forceNormalizedManualFingerings, 1);
    EXPECT_GT(std::abs(*forceManual.yOffset - *prettifyManual.yOffset), 0.05);
    EXPECT_NE(forceManual.pmxy, prettifyManual.pmxy);
}

TEST_F(Mei_Tests, mei_ftrem_01) {
    meiReadTest("ftrem-01");
}

TEST_F(Mei_Tests, mei_glisss_01) {
    meiReadTest("gliss-01");
}

TEST_F(Mei_Tests, mei_gracenote_01) {
    meiReadTest("gracenote-01");
}

TEST_F(Mei_Tests, mei_gracenote_02) {
    meiReadTest("gracenote-02");
}

TEST_F(Mei_Tests, mei_hairpin_01) {
    meiReadTest("hairpin-01");
}

TEST_F(Mei_Tests, mei_hairpin_export_includes_pm_hairpin_lines_when_endpoints_present) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"hairpin-01.mscx", false);
    ASSERT_TRUE(score);

    score->style().set(Sid::dynamicsHairpinsAutoCenterOnGrandStaff, true);
    score->setLayoutAll();
    score->doLayout();

    const String outputName = u"hairpin-01.pm-lines.test.mei";
    bool output = ScoreRW::saveScore(score, outputName, exportFunc);
    EXPECT_TRUE(output);
    delete score;

    muse::io::File outputFile(outputName);
    ASSERT_TRUE(outputFile.open(muse::io::IODevice::ReadOnly));
    auto outputData = outputFile.readAll();
    std::string meiText(reinterpret_cast<const char*>(outputData.constData()), outputData.size());

    size_t hairpinTagCount = 0;
    size_t inspectedTagCount = 0;
    size_t cursor = 0;

    while ((cursor = meiText.find("<hairpin", cursor)) != std::string::npos) {
        size_t end = meiText.find('>', cursor);
        ASSERT_NE(end, std::string::npos);

        std::string tag = meiText.substr(cursor, end - cursor + 1);
        hairpinTagCount++;

        if (tag.find("pm:x1y1x2y2=") != std::string::npos) {
            inspectedTagCount++;
            EXPECT_NE(tag.find("pm:hairpin-lines="), std::string::npos);
        }

        cursor = end + 1;
    }

    EXPECT_GT(hairpinTagCount, 0u);
    EXPECT_GE(hairpinTagCount, inspectedTagCount);

    size_t centeredHairpinCount = 0;
    size_t nonCenteredHairpinCount = 0;
    for (const std::string& tag : collectStartTags(meiText, "hairpin")) {
        const std::optional<std::string> centered = xmlAttributeValue(tag, "centerBetweenStaves");
        ASSERT_TRUE(centered.has_value()) << tag;
        if (centered == "true") {
            centeredHairpinCount++;
        } else if (centered == "false") {
            nonCenteredHairpinCount++;
        }
    }
    EXPECT_GT(centeredHairpinCount, 0u);
    EXPECT_GT(nonCenteredHairpinCount, 0u);
}

TEST_F(Mei_Tests, mei_line_hairpin_exports_resolved_center_and_layout_y_offset) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"hairpin-01.mscx", false);
    ASSERT_TRUE(score);

    Hairpin* lineHairpin = nullptr;
    for (const auto& pair : score->spanner()) {
        Spanner* spanner = pair.second;
        if (!spanner || !spanner->isHairpin()) {
            continue;
        }

        Hairpin* hairpin = toHairpin(spanner);
        if (hairpin->staffIdx() == 0 && !hairpin->isLineType()) {
            lineHairpin = hairpin;
            break;
        }
    }
    ASSERT_TRUE(lineHairpin);

    lineHairpin->setHairpinType(HairpinType::CRESC_LINE);
    lineHairpin->setCenterBetweenStaves(AutoOnOff::AUTO);
    score->style().set(Sid::dynamicsHairpinsAutoCenterOnGrandStaff, true);
    score->setLayoutAll();
    score->doLayout();

    ASSERT_FALSE(lineHairpin->segmentsEmpty());
    const SpannerSegment* firstSegment = lineHairpin->frontSegment();
    ASSERT_TRUE(firstSegment);
    const System* system = firstSegment->system();
    ASSERT_TRUE(system);
    const Staff* staff = lineHairpin->staff();
    ASSERT_TRUE(staff);

    const double spatium = staff->spatium(lineHairpin->tick());
    ASSERT_GT(spatium, 0.0);
    double staffY = system->staffYpage(staff->idx());
    if (lineHairpin->placement() == PlacementV::BELOW) {
        staffY += system->staff(staff->idx())->bbox().height();
    }
    staffY += lineHairpin->staffOffsetY();
    const double expectedYOffset = (staffY - firstSegment->pagePos().y()) / spatium;
    EXPECT_LT(expectedYOffset, 0.0);

    const String outputName = u"hairpin-01.line-center.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> directiveTags = collectStartTags(meiText, "dir");

    size_t matchingTagCount = 0;
    for (const std::string& tag : directiveTags) {
        const std::optional<std::string> type = xmlAttributeValue(tag, "type");
        if (!type.has_value() || type->find("mscore-hairpin") == std::string::npos) {
            continue;
        }

        const std::optional<double> yOffset = xmlAttributeDouble(tag, "yOffset");
        if (xmlAttributeValue(tag, "centerBetweenStaves") != "true"
            || !yOffset.has_value()
            || std::abs(*yOffset - expectedYOffset) > 0.051) {
            continue;
        }

        matchingTagCount++;
        EXPECT_LT(*yOffset, 0.0);
        EXPECT_TRUE(xmlAttributeValue(tag, "pm:xy").has_value());
        EXPECT_TRUE(xmlAttributeValue(tag, "pm:x1y1x2y2").has_value());
        EXPECT_TRUE(xmlAttributeValue(tag, "pm:segments").has_value());
    }

    EXPECT_EQ(matchingTagCount, 1u);
}

TEST_F(Mei_Tests, mei_directives_export_resolved_center_between_staves) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"hairpin-01.mscx", false);
    ASSERT_TRUE(score);

    Segment* firstSegment = score->firstSegment(SegmentType::ChordRest);
    ASSERT_TRUE(firstSegment);

    Expression* expression = Factory::createExpression(firstSegment, true);
    expression->setTrack(0);
    expression->setXmlText(u"centered expression");
    expression->setCenterBetweenStaves(AutoOnOff::ON);
    expression->setVoiceAssignment(VoiceAssignment::ALL_VOICE_IN_INSTRUMENT);
    firstSegment->add(expression);

    Dynamic* customDynamic = Factory::createDynamic(firstSegment, true);
    customDynamic->setTrack(0);
    customDynamic->setDynamicType(DynamicType::OTHER);
    customDynamic->setXmlText(u"centered custom dynamic");
    customDynamic->setCenterBetweenStaves(AutoOnOff::ON);
    customDynamic->setVoiceAssignment(VoiceAssignment::ALL_VOICE_IN_INSTRUMENT);
    firstSegment->add(customDynamic);

    StaffText* staffText = Factory::createStaffText(firstSegment, TextStyleType::STAFF, true);
    staffText->setTrack(0);
    staffText->setXmlText(u"staff-relative directive");
    staffText->setPlacement(PlacementV::BELOW);
    staffText->setOffset(0.0, staffText->spatium());
    firstSegment->add(staffText);

    std::vector<Dynamic*> standardDynamics;
    for (Segment* segment = firstSegment; segment; segment = segment->next1()) {
        for (EngravingItem* annotation : segment->annotations()) {
            if (annotation && annotation->isDynamic()) {
                Dynamic* dynamic = toDynamic(annotation);
                if (dynamic->dynamicType() != DynamicType::OTHER) {
                    standardDynamics.push_back(dynamic);
                }
            }
        }
    }
    ASSERT_GE(standardDynamics.size(), 2u);
    standardDynamics[0]->setCenterBetweenStaves(AutoOnOff::ON);
    standardDynamics[1]->setCenterBetweenStaves(AutoOnOff::OFF);

    score->style().set(Sid::dynamicsHairpinsAutoCenterOnGrandStaff, true);
    score->setLayoutAll();
    score->doLayout();

    const Staff* directiveStaff = staffText->staff();
    ASSERT_TRUE(directiveStaff);
    const StaffType* directiveStaffType = directiveStaff->staffTypeForElement(staffText);
    ASSERT_TRUE(directiveStaffType);
    const double directiveLineDistance = directiveStaff->spatium(staffText->tick())
        * directiveStaffType->lineDistance().val();
    ASSERT_GT(directiveLineDistance, 0.0);
    const double expectedStaffTextYOffset =
        (((directiveStaffType->lines() - 1) * directiveLineDistance) - staffText->y())
        / directiveLineDistance;

    const String outputName = u"hairpin-01.directive-center.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> directiveTags = collectStartTags(meiText, "dir");
    size_t centeredOrdinaryDirectives = 0;
    size_t staffRelativeDirectives = 0;
    for (const std::string& tag : directiveTags) {
        const std::optional<std::string> type = xmlAttributeValue(tag, "type");
        if (type == "mscore-staff-text") {
            EXPECT_FALSE(xmlAttributeValue(tag, "centerBetweenStaves").has_value()) << tag;
            const std::optional<double> yOffset = xmlAttributeDouble(tag, "yOffset");
            ASSERT_TRUE(yOffset.has_value()) << tag;
            EXPECT_NEAR(*yOffset, expectedStaffTextYOffset, 0.051);
            staffRelativeDirectives++;
            continue;
        }
        if (type.has_value() && type != "mscore-") {
            continue;
        }
        EXPECT_EQ(xmlAttributeValue(tag, "centerBetweenStaves"), "true") << tag;
        centeredOrdinaryDirectives++;
    }
    EXPECT_EQ(centeredOrdinaryDirectives, 2u);
    EXPECT_EQ(staffRelativeDirectives, 1u);

    size_t centeredDynamics = 0;
    size_t nonCenteredDynamics = 0;
    for (const std::string& tag : collectStartTags(meiText, "dynam")) {
        const std::optional<std::string> centered = xmlAttributeValue(tag, "centerBetweenStaves");
        ASSERT_TRUE(centered.has_value()) << tag;
        if (centered == "true") {
            centeredDynamics++;
        } else if (centered == "false") {
            nonCenteredDynamics++;
        }
    }
    EXPECT_GE(centeredDynamics, 1u);
    EXPECT_GE(nonCenteredDynamics, 1u);
}

TEST_F(Mei_Tests, mei_export_omits_invisible_note_and_idx) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"beam-01.mscx", false);
    ASSERT_TRUE(score);

    const String baselineOutputName = u"beam-01.visibility-baseline.test.mei";
    const String modifiedOutputName = u"beam-01.visibility-modified.test.mei";

    bool baselineOutput = ScoreRW::saveScore(score, baselineOutputName, exportFunc);
    ASSERT_TRUE(baselineOutput);

    Note* firstExportableNote = nullptr;
    for (Segment* segment = score->firstSegment(SegmentType::ChordRest); segment && !firstExportableNote; segment = segment->next1()) {
        for (track_idx_t track = 0; track < score->ntracks() && !firstExportableNote; ++track) {
            EngravingItem* item = segment->element(track);
            if (!item || !item->isChord()) {
                continue;
            }

            Chord* chord = toChord(item);
            for (Note* note : chord->notes()) {
                if (!note->visible()) {
                    continue;
                }

                firstExportableNote = note;
                break;
            }
        }
    }
    ASSERT_TRUE(firstExportableNote);

    firstExportableNote->setVisible(false);

    bool modifiedOutput = ScoreRW::saveScore(score, modifiedOutputName, exportFunc);
    ASSERT_TRUE(modifiedOutput);
    delete score;

    auto readFile = [](const String& fileName) {
        muse::io::File file(fileName);
        EXPECT_TRUE(file.open(muse::io::IODevice::ReadOnly));
        if (!file.isOpen()) {
            return std::string();
        }

        auto data = file.readAll();
        return std::string(reinterpret_cast<const char*>(data.constData()), data.size());
    };

    const std::string baselineText = readFile(baselineOutputName);
    const std::string modifiedText = readFile(modifiedOutputName);

    auto countOccurrences = [](const std::string& text, const std::string& needle) {
        size_t count = 0;
        size_t cursor = 0;
        while ((cursor = text.find(needle, cursor)) != std::string::npos) {
            ++count;
            cursor += needle.size();
        }
        return count;
    };

    const size_t baselineNoteCount = countOccurrences(baselineText, "<note");
    const size_t modifiedNoteCount = countOccurrences(modifiedText, "<note");
    const size_t baselineIdxCount = countOccurrences(baselineText, "idx=\"");
    const size_t modifiedIdxCount = countOccurrences(modifiedText, "idx=\"");

    ASSERT_GT(baselineNoteCount, 0u);
    ASSERT_GT(baselineIdxCount, 0u);
    EXPECT_EQ(modifiedNoteCount, baselineNoteCount - 1);
    EXPECT_EQ(modifiedIdxCount, baselineIdxCount - 1);

    MasterScore* spannerScore = ScoreRW::readScore(MEI_DIR + u"color-01.mscx", false);
    ASSERT_TRUE(spannerScore);

    size_t mixedVisibilityChordCount = 0;
    for (Segment* segment = spannerScore->firstSegment(SegmentType::ChordRest); segment; segment = segment->next1()) {
        for (track_idx_t track = 0; track < spannerScore->ntracks(); ++track) {
            EngravingItem* item = segment->element(track);
            if (!item || !item->isChord()) {
                continue;
            }

            Chord* chord = toChord(item);
            if (chord->notes().empty()) {
                continue;
            }

            Note* sourceNote = chord->notes().front();
            Note* invisibleNote = Factory::createNote(chord);
            invisibleNote->setPitch(sourceNote->pitch(), sourceNote->tpc1(), sourceNote->tpc2());
            invisibleNote->setVisible(false);
            chord->add(invisibleNote);
            ++mixedVisibilityChordCount;
        }
    }
    ASSERT_GT(mixedVisibilityChordCount, 0u);

    const String spannerOutputName = u"color-01.visibility-spanners.test.mei";
    bool spannerOutput = ScoreRW::saveScore(spannerScore, spannerOutputName, exportFunc);
    ASSERT_TRUE(spannerOutput);
    delete spannerScore;

    const std::string spannerText = readFile(spannerOutputName);
    std::set<std::string> declaredIds;
    size_t declarationCursor = 0;
    const std::string declarationPrefix = "xml:id=\"";
    while ((declarationCursor = spannerText.find(declarationPrefix, declarationCursor)) != std::string::npos) {
        size_t valueStart = declarationCursor + declarationPrefix.size();
        size_t valueEnd = spannerText.find('"', valueStart);
        ASSERT_NE(valueEnd, std::string::npos);
        declaredIds.insert(spannerText.substr(valueStart, valueEnd - valueStart));
        declarationCursor = valueEnd + 1;
    }

    const std::vector<std::string> identityAttributes {
        "pm:covered-id", "pm:covered-ids", "pm:coveredUuids",
        "pm:post-terminal-ids", "pm:terminal-ids"
    };
    std::set<std::string> observedAttributes;
    for (const std::string& attributeName : identityAttributes) {
        const std::string attributePrefix = attributeName + "=\"";
        size_t attributeCursor = 0;
        while ((attributeCursor = spannerText.find(attributePrefix, attributeCursor)) != std::string::npos) {
            observedAttributes.insert(attributeName);
            size_t valueStart = attributeCursor + attributePrefix.size();
            size_t valueEnd = spannerText.find('"', valueStart);
            ASSERT_NE(valueEnd, std::string::npos);

            std::istringstream tokens(spannerText.substr(valueStart, valueEnd - valueStart));
            std::string token;
            while (tokens >> token) {
                EXPECT_NE(declaredIds.find(token), declaredIds.end())
                    << attributeName << " references undeclared " << token;
            }
            attributeCursor = valueEnd + 1;
        }
    }

    EXPECT_NE(observedAttributes.find("pm:coveredUuids"), observedAttributes.end());
    EXPECT_NE(observedAttributes.find("pm:covered-id"), observedAttributes.end());
    EXPECT_TRUE(observedAttributes.find("pm:covered-ids") != observedAttributes.end()
                || observedAttributes.find("pm:terminal-ids") != observedAttributes.end()
                || observedAttributes.find("pm:post-terminal-ids") != observedAttributes.end());
}

TEST_F(Mei_Tests, mei_export_rehearsal_mark_on_hidden_rest) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"rehearsal-hidden-rest-01.mscx", false);
    ASSERT_TRUE(score);

    const String outputName = u"rehearsal-hidden-rest-01.test.mei";
    bool output = ScoreRW::saveScore(score, outputName, exportFunc);
    ASSERT_TRUE(output);
    delete score;

    muse::io::File outputFile(outputName);
    ASSERT_TRUE(outputFile.open(muse::io::IODevice::ReadOnly));
    auto outputData = outputFile.readAll();
    std::string meiText(reinterpret_cast<const char*>(outputData.constData()), outputData.size());

    size_t rehStart = meiText.find("<reh");
    ASSERT_NE(rehStart, std::string::npos);
    size_t rehEnd = meiText.find("</reh>", rehStart);
    ASSERT_NE(rehEnd, std::string::npos);

    std::string rehElement = meiText.substr(rehStart, rehEnd - rehStart + 6);
    EXPECT_NE(rehElement.find("startid=\"#"), std::string::npos);
    EXPECT_NE(rehElement.find(">HiddenRestMark</reh>"), std::string::npos);
}

TEST_F(Mei_Tests, mei_export_spanners_on_hidden_rests_use_visible_same_staff_notes) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-spanners-on-hidden-rests.mscx", false);
    ASSERT_TRUE(score);

    score->setLayoutAll();
    score->doLayout();

    const String outputName = u"pianomania-spanners-on-hidden-rests.test.mei";
    ASSERT_TRUE(ScoreRW::saveScore(score, outputName, exportFunc));
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> noteTags = collectStartTags(meiText, "note");
    const std::vector<std::string> hairpinTags = collectStartTags(meiText, "hairpin");
    const std::vector<std::string> octaveTags = collectStartTags(meiText, "octave");
    ASSERT_EQ(noteTags.size(), 4u);
    ASSERT_EQ(hairpinTags.size(), 1u);
    ASSERT_EQ(octaveTags.size(), 1u);

    const auto noteIdForPitch = [&](const std::string& pname, const std::string& oct) {
        std::optional<std::string> matchingId;
        for (const std::string& noteTag : noteTags) {
            if (xmlAttributeValue(noteTag, "pname") != pname
                || xmlAttributeValue(noteTag, "oct") != oct) {
                continue;
            }
            EXPECT_FALSE(matchingId.has_value()) << noteTag;
            matchingId = xmlAttributeValue(noteTag, "xml:id");
        }
        return matchingId;
    };

    const std::optional<std::string> firstNoteId = noteIdForPitch("c", "5");
    const std::optional<std::string> secondNoteId = noteIdForPitch("d", "5");
    const std::optional<std::string> ottavaEndNoteId = noteIdForPitch("c", "4");
    ASSERT_TRUE(firstNoteId.has_value()) << noteTags[0];
    ASSERT_TRUE(secondNoteId.has_value()) << noteTags[1];
    ASSERT_TRUE(ottavaEndNoteId.has_value()) << noteTags[2];
    ASSERT_NE(*firstNoteId, *secondNoteId);

    const auto expectAnchors = [&](const std::string& spannerTag, const std::string& expectedEndId) {
        const std::optional<std::string> startId = xmlAttributeValue(spannerTag, "startid");
        const std::optional<std::string> endId = xmlAttributeValue(spannerTag, "endid");
        ASSERT_TRUE(startId.has_value()) << spannerTag;
        ASSERT_TRUE(endId.has_value()) << spannerTag;
        ASSERT_GT(startId->size(), 1u);
        ASSERT_GT(endId->size(), 1u);
        EXPECT_EQ(startId->substr(1), *firstNoteId) << spannerTag;
        EXPECT_EQ(endId->substr(1), expectedEndId) << spannerTag;
    };

    expectAnchors(hairpinTags.front(), *secondNoteId);
    expectAnchors(octaveTags.front(), *ottavaEndNoteId);
}

TEST_F(Mei_Tests, mei_pianomania_grace_same_pitch_indices) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-grace-same-pitch-index.mscx", false);
    ASSERT_TRUE(score);

    const String outputName = u"pianomania-grace-same-pitch-index.test.mei";
    bool output = ScoreRW::saveScore(score, outputName, exportFunc);
    ASSERT_TRUE(output);
    delete score;

    muse::io::File file(outputName);
    ASSERT_TRUE(file.open(muse::io::IODevice::ReadOnly));
    auto data = file.readAll();
    std::string meiText(reinterpret_cast<const char*>(data.constData()), data.size());

    std::vector<std::string> gSharpTags;
    size_t cursor = 0;
    while ((cursor = meiText.find("<note", cursor)) != std::string::npos) {
        size_t end = meiText.find('>', cursor);
        ASSERT_NE(end, std::string::npos);

        std::string tag = meiText.substr(cursor, end - cursor + 1);
        if (tag.find("pname=\"g\"") != std::string::npos && tag.find("oct=\"4\"") != std::string::npos) {
            gSharpTags.push_back(tag);
        }
        cursor = end + 1;
    }

    ASSERT_EQ(gSharpTags.size(), 3u);
    EXPECT_NE(gSharpTags[0].find("idx=\"0\""), std::string::npos);
    EXPECT_NE(gSharpTags[1].find("idx=\"1\""), std::string::npos);
    EXPECT_NE(gSharpTags[2].find("idx=\"2\""), std::string::npos);
}

TEST_F(Mei_Tests, mei_harp_01) {
    meiReadTest("harp-01");
}

TEST_F(Mei_Tests, mei_jump_01) {
    meiReadTest("jump-01");
}

TEST_F(Mei_Tests, mei_jump_02) {
    meiReadTest("jump-02");
}

TEST_F(Mei_Tests, mei_key_signature_01) {
    meiReadTest("key-signature-01");
}

TEST_F(Mei_Tests, mei_midi_01) {
    meiReadTest("midi-01");
}

TEST_F(Mei_Tests, mei_label_01) {
    meiReadTest("label-01");
}

TEST_F(Mei_Tests, laissez_vibrer_01) {
    meiReadTest("laissez-vibrer-01");
}

TEST_F(Mei_Tests, mei_export_laissez_vibrer_keeps_pm_geometry) {
    auto importFunc = [](MasterScore* score, const muse::io::path_t& path) -> Err {
        MeiReader meiReader(nullptr);
        return meiReader.import(score, path);
    };
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"laissez-vibrer-01.mei", false, importFunc);
    ASSERT_TRUE(score);

    ASSERT_TRUE(ScoreRW::saveScore(score, u"laissez-vibrer-01.setup.mscx"));

    const String outputName = u"laissez-vibrer-01.pm-geometry.test.mei";
    bool output = ScoreRW::saveScore(score, outputName, exportFunc);
    ASSERT_TRUE(output);
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> laissezVibrerTags = collectStartTags(meiText, "lv");

    ASSERT_EQ(laissezVibrerTags.size(), 6u);
    for (const std::string& tag : laissezVibrerTags) {
        EXPECT_TRUE(xmlAttributeValue(tag, "startid").has_value());
        EXPECT_TRUE(xmlAttributeValue(tag, "pm:x1y1x2y2").has_value());
        EXPECT_TRUE(xmlAttributeValue(tag, "pm:bezier").has_value());
    }
}

TEST_F(Mei_Tests, mei_lyric_01) {
    meiReadTest("lyric-01");
}

TEST_F(Mei_Tests, mei_lyric_02) {
    meiReadTest("lyric-02");
}

TEST_F(Mei_Tests, mei_lyric_03) {
    meiReadTest("lyric-03");
}

TEST_F(Mei_Tests, mei_lyric_04) {
    meiReadTest("lyric-04");
}

TEST_F(Mei_Tests, mei_measure_01) {
    meiReadTest("measure-01");
}

TEST_F(Mei_Tests, mei_measure_02) {
    meiReadTest("measure-02");
}

TEST_F(Mei_Tests, mei_mrpt_01) {
    meiReadTest("measure-repeat-01");
}

TEST_F(Mei_Tests, mei_metadata_01) {
    meiReadTest("metadata-01");
}

TEST_F(Mei_Tests, mei_mordent_01) {
    meiReadTest("mordent-01");
}

TEST_F(Mei_Tests, mei_octave_01) {
    meiReadTest("octave-01");
}

TEST_F(Mei_Tests, mei_ornam_01) {
    meiReadTest("ornam-01");
}

TEST_F(Mei_Tests, mei_page_head_01) {
    meiReadTest("page-head-01");
}

TEST_F(Mei_Tests, mei_page_head_02) {
    meiReadTest("page-head-02");
}

TEST_F(Mei_Tests, mei_pedal_01) {
    meiReadTest("pedal-01");
}

TEST_F(Mei_Tests, mei_export_unresolved_pedal_endpoint_omits_pm_geometry) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pedal-unresolved-end-01.mscx", false);
    ASSERT_TRUE(score);

    const String outputName = u"pedal-unresolved-end-01.test.mei";
    bool output = ScoreRW::saveScore(score, outputName, exportFunc);
    ASSERT_TRUE(output);
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> pedalTags = collectStartTags(meiText, "pedal");

    ASSERT_GT(pedalTags.size(), 0u);

    size_t unresolvedTagCount = 0;
    size_t geometryTagCount = 0;
    for (const std::string& tag : pedalTags) {
        if (xmlAttributeValue(tag, "pm:x1y1x2y2").has_value()) {
            geometryTagCount++;
            continue;
        }

        unresolvedTagCount++;
        EXPECT_TRUE(xmlAttributeValue(tag, "startid").has_value());
        EXPECT_FALSE(xmlAttributeValue(tag, "endid").has_value());
    }

    EXPECT_GT(unresolvedTagCount, 0u);
    EXPECT_GT(geometryTagCount, 0u);
}

TEST_F(Mei_Tests, mei_export_connected_pedal_keeps_pm_geometry) {
    auto exportFunc = [](Score* score, const muse::io::path_t& path) -> Err {
        MeiWriter meiWriter;
        return meiWriter.writeScore(score, path);
    };

    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pedal-01.mscx", false);
    ASSERT_TRUE(score);

    const String outputName = u"pedal-01.pm-geometry.test.mei";
    bool output = ScoreRW::saveScore(score, outputName, exportFunc);
    ASSERT_TRUE(output);
    delete score;

    const std::string meiText = readTestTextFile(outputName);
    const std::vector<std::string> pedalTags = collectStartTags(meiText, "pedal");

    ASSERT_GT(pedalTags.size(), 0u);

    size_t geometryTagCount = 0;
    for (const std::string& tag : pedalTags) {
        if (xmlAttributeValue(tag, "pm:x1y1x2y2").has_value()) {
            geometryTagCount++;
            EXPECT_TRUE(xmlAttributeValue(tag, "endid").has_value());
        }
    }

    EXPECT_GT(geometryTagCount, 0u);
}

TEST_F(Mei_Tests, pianomania_graces_do_not_duplicate_parent_segment_controls) {
    MasterScore* score = ScoreRW::readScore(MEI_DIR + u"pianomania-grace-same-pitch-index.mscx", false);
    ASSERT_TRUE(score);

    std::string meiText;
    ASSERT_TRUE(pmWriteMeiToString(score, true, meiText));
    delete score;

    std::vector<std::string> ids;
    const std::string needle = "xml:id=\"";
    size_t cursor = 0;
    while ((cursor = meiText.find(needle, cursor)) != std::string::npos) {
        cursor += needle.size();
        const size_t end = meiText.find('"', cursor);
        ASSERT_NE(end, std::string::npos);
        ids.push_back(meiText.substr(cursor, end - cursor));
        cursor = end + 1;
    }

    ASSERT_FALSE(ids.empty());
    const std::set<std::string> uniqueIds(ids.begin(), ids.end());
    EXPECT_EQ(uniqueIds.size(), ids.size());
    const std::vector<std::string> dynamicTags = collectStartTags(meiText, "dynam");
    ASSERT_EQ(dynamicTags.size(), 1u);

    const std::vector<std::string> directiveTags = collectStartTags(meiText, "dir");
    ASSERT_EQ(directiveTags.size(), 2u);
    EXPECT_NE(meiText.find(">dolce</dir>"), std::string::npos);
    EXPECT_NE(meiText.find(">legato</dir>"), std::string::npos);
    for (const std::string& directiveTag : directiveTags) {
        EXPECT_TRUE(xmlAttributeValue(directiveTag, "startid").has_value());
        EXPECT_TRUE(xmlAttributeValue(directiveTag, "yOffset").has_value());
        EXPECT_TRUE(xmlAttributeValue(directiveTag, "pm:xy").has_value());
    }

    const std::vector<std::string> noteTags = collectStartTags(meiText, "note");
    ASSERT_EQ(noteTags.size(), 4u);
    const std::optional<std::string> parentId = xmlAttributeValue(noteTags[1], "xml:id");
    ASSERT_TRUE(parentId.has_value());
    EXPECT_EQ(xmlAttributeValue(dynamicTags[0], "startid"), "#" + parentId.value());

    const std::vector<std::string> fingeringTags = collectStartTags(meiText, "fing");
    ASSERT_EQ(fingeringTags.size(), 1u);
    const std::optional<std::string> graceId = xmlAttributeValue(noteTags[3], "xml:id");
    ASSERT_TRUE(graceId.has_value());
    EXPECT_EQ(xmlAttributeValue(fingeringTags[0], "startid"), "#" + graceId.value());
}

TEST_F(Mei_Tests, mei_reh_01) {
    meiReadTest("reh-01");
}

TEST_F(Mei_Tests, mei_roman_numeral_01) {
    meiReadTest("roman-numeral-01");
}

TEST_F(Mei_Tests, mei_score_01) {
    meiReadTest("score-01");
}

TEST_F(Mei_Tests, mei_score_02) {
    meiReadTest("score-02");
}

TEST_F(Mei_Tests, mei_score_03) {
    meiReadTest("score-03");
}

TEST_F(Mei_Tests, mei_slur_01) {
    meiReadTest("slur-01");
}

TEST_F(Mei_Tests, mei_slur_02) {
    meiReadTest("slur-02");
}

TEST_F(Mei_Tests, mei_stem_01) {
    meiReadTest("stem-01");
}

TEST_F(Mei_Tests, mei_tempo_01) {
    meiReadTest("tempo-01");
}

TEST_F(Mei_Tests, mei_tie_01) {
    meiReadTest("tie-01");
}

TEST_F(Mei_Tests, mei_time_signature_01) {
    meiReadTest("time-signature-01");
}

TEST_F(Mei_Tests, mei_time_signature_02) {
    meiReadTest("time-signature-02");
}

TEST_F(Mei_Tests, mei_transpose_01) {
    meiReadTest("transpose-01");
}

TEST_F(Mei_Tests, mei_trill_01) {
    meiReadTest("trill-01");
}

TEST_F(Mei_Tests, mei_tuplet_01) {
    meiReadTest("tuplet-01");
}

TEST_F(Mei_Tests, mei_tuplet_02) {
    meiReadTest("tuplet-02");
}

TEST_F(Mei_Tests, mei_tuplet_03) {
    meiReadTest("tuplet-03");
}
}
