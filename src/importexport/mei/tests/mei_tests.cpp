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

#include <gtest/gtest.h>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "io/file.h"

#include "engraving/tests/utils/scorerw.h"
#include "engraving/tests/utils/scorecomp.h"

#include "engraving/dom/masterscore.h"
#include "engraving/dom/excerpt.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/segment.h"

#include "modularity/ioc.h"
#include "importexport/mei/imeiconfiguration.h"
#include "importexport/mei/internal/meireader.h"
#include "importexport/mei/internal/meiwriter.h"

using namespace mu;
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
        MeiReader meiReader;
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

TEST_F(Mei_Tests, mei_beam_01) {
    meiReadTest("beam-01");
}

TEST_F(Mei_Tests, mei_beam_02) {
    meiReadTest("beam-02");
}

TEST_F(Mei_Tests, mei_beam_03) {
    meiReadTest("beam-03");
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
