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

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTemporaryDir>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/mscore.h"
#include "engraving/tests/utils/scorerw.h"
#include "project/internal/pianomaniaexport.h"

namespace {
constexpr const char* MANIFEST_SCHEMA = "a2p.exporter-provenance.v2";

struct FileExpectation {
    const char* kind = nullptr;
    int audibleEvents = 0;
    int visualOccurrences = 0;
    int ornamentOccurrences = 0;
};

struct ExportCase {
    const char* name = nullptr;
    const char* fixture = nullptr;
    bool hasRepeats = false;
    bool hasMultipleEndings = false;
    int finalEndingNumber = 0;
    int canonicalEvents = 0;
    int writtenGraceEvents = 0;
    int sameOnsetAliases = 0;
    int tieAliases = 0;
    int ornamentDefinitions = 0;
    std::vector<FileExpectation> files;
};

class ScopedMScoreTestMode
{
public:
    explicit ScopedMScoreTestMode(bool value)
        : m_previous(mu::engraving::MScore::testMode)
    {
        mu::engraving::MScore::testMode = value;
    }

    ~ScopedMScoreTestMode()
    {
        mu::engraving::MScore::testMode = m_previous;
    }

private:
    bool m_previous = false;
};

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

int countCanonicalEvents(const QJsonArray& events, const char* key, const char* value)
{
    int count = 0;
    for (const QJsonValue& valueObject : events) {
        if (valueObject.toObject().value(key).toString() == value) {
            ++count;
        }
    }
    return count;
}

void verifyLocator(const QJsonObject& locator)
{
    EXPECT_TRUE(locator.value("track").isDouble());
    EXPECT_TRUE(locator.value("event_ordinal").isDouble());
    EXPECT_TRUE(locator.value("absolute_tick").isDouble());
    EXPECT_TRUE(locator.value("status").isDouble());
    EXPECT_TRUE(locator.value("channel").isDouble());
    ASSERT_TRUE(locator.value("data").isArray());
    EXPECT_EQ(locator.value("data").toArray().size(), 2);
}

void verifyOccurrenceLinkage(const QJsonObject& file,
                             const std::set<QString>& canonicalIds,
                             const std::set<QString>& ornamentDefinitionIds,
                             const std::set<QString>& expectedAliasIds)
{
    const QJsonArray visual = file.value("visual_occurrences").toArray();
    const QJsonArray ornaments = file.value("ornament_occurrences").toArray();
    const QJsonArray audible = file.value("audible_events").toArray();

    std::set<QString> occurrenceIds;
    std::set<QString> audibleIds;
    std::set<QString> actualAliasIds;
    std::map<QString, std::set<QString> > declaredOccurrences;

    for (const QJsonValue& value : audible) {
        const QJsonObject event = value.toObject();
        const QString audibleId = event.value("audible_event_id").toString();
        EXPECT_FALSE(audibleId.isEmpty());
        EXPECT_TRUE(audibleIds.insert(audibleId).second);
        verifyLocator(event.value("note_on_locator").toObject());
        verifyLocator(event.value("note_off_locator").toObject());

        const bool ornament = event.value("event_type").toString() == "nonvisual_ornament";
        if (ornament) {
            EXPECT_TRUE(event.value("idx").isNull());
            EXPECT_EQ(event.value("note_off_velocity").toInt(), 127);
            EXPECT_EQ(event.value("ornament_provenance").toString(), "score_declared");
            EXPECT_FALSE(event.value("ornament_definition_id").toString().isEmpty());
        } else {
            EXPECT_TRUE(event.value("idx").isDouble());
            EXPECT_NE(event.value("note_off_velocity").toInt(), 127);
            EXPECT_TRUE(event.value("ornament_definition_id").isNull());
        }

        for (const QJsonValue& occurrence : event.value("occurrence_ids").toArray()) {
            declaredOccurrences[audibleId].insert(occurrence.toString());
        }
        for (const QJsonValue& alias : event.value("alias_ids").toArray()) {
            const QString aliasId = alias.toString();
            EXPECT_TRUE(expectedAliasIds.count(aliasId));
            actualAliasIds.insert(aliasId);
        }
    }

    const auto verifyOccurrence = [&](const QJsonValue& value, bool ornament) {
        const QJsonObject occurrence = value.toObject();
        const QString occurrenceId = occurrence.value("occurrence_id").toString();
        const QString audibleId = occurrence.value("audible_event_id").toString();
        EXPECT_FALSE(occurrenceId.isEmpty());
        EXPECT_TRUE(occurrenceIds.insert(occurrenceId).second);
        EXPECT_TRUE(audibleIds.count(audibleId));
        EXPECT_TRUE(declaredOccurrences[audibleId].count(occurrenceId));
        if (ornament) {
            EXPECT_TRUE(ornamentDefinitionIds.count(occurrence.value("ornament_definition_id").toString()));
        } else {
            EXPECT_TRUE(canonicalIds.count(occurrence.value("score_event_id").toString()));
        }
    };
    for (const QJsonValue& occurrence : visual) {
        verifyOccurrence(occurrence, false);
    }
    for (const QJsonValue& occurrence : ornaments) {
        verifyOccurrence(occurrence, true);
    }

    size_t declaredCount = 0;
    for (const auto& [audibleId, ids] : declaredOccurrences) {
        EXPECT_TRUE(audibleIds.count(audibleId));
        declaredCount += ids.size();
    }
    EXPECT_EQ(declaredCount, occurrenceIds.size());
    EXPECT_EQ(actualAliasIds.size(), expectedAliasIds.size());
    for (const QString& aliasId : expectedAliasIds) {
        EXPECT_TRUE(actualAliasIds.count(aliasId));
    }
}
}

class Project_PianomaniaExportTest : public ::testing::TestWithParam<ExportCase>
{
};

TEST_P(Project_PianomaniaExportTest, ManifestV2FixtureMatrix)
{
    // MasterScore::clone() serializes through the normal score writer. Exercise
    // its production EID path so no-repeat variants retain canonical identities.
    ScopedMScoreTestMode productionScoreSerialization(false);

    const ExportCase& expectation = GetParam();
    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/" + muse::String::fromUtf8(expectation.fixture);
    std::unique_ptr<mu::engraving::MasterScore> score(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);

    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    ASSERT_TRUE(firstDirectory.isValid());
    ASSERT_TRUE(secondDirectory.isValid());

    const auto exportTo = [&](const QTemporaryDir& directory) {
        const muse::io::path_t basePath(directory.filePath("song"));
        const muse::io::path_t meiPath(directory.filePath("song.mei"));
        return mu::project::pianomania::exportPianomaniaBundle(
            score.get(), muse::io::path_t(fixturePath), basePath, meiPath, false);
    };

    muse::RetVal<mu::project::pianomania::PianomaniaExportResult> first = exportTo(firstDirectory);
    ASSERT_TRUE(first.ret) << first.ret.toString();
    ASSERT_TRUE(mu::project::pianomania::writePianomaniaManifest(
        muse::io::path_t(firstDirectory.filePath("manifest.json")), first.val));

    muse::RetVal<mu::project::pianomania::PianomaniaExportResult> second = exportTo(secondDirectory);
    ASSERT_TRUE(second.ret) << second.ret.toString();
    ASSERT_TRUE(mu::project::pianomania::writePianomaniaManifest(
        muse::io::path_t(secondDirectory.filePath("manifest.json")), second.val));

    EXPECT_EQ(first.val.manifestJson, second.val.manifestJson);
    EXPECT_EQ(readFile(firstDirectory.filePath("song.mei")), readFile(secondDirectory.filePath("song.mei")));
    EXPECT_EQ(readFile(firstDirectory.filePath("manifest.json")), readFile(secondDirectory.filePath("manifest.json")));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(first.val.manifestJson), &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError) << parseError.errorString().toStdString();
    ASSERT_TRUE(document.isObject());
    const QJsonObject root = document.object();
    EXPECT_EQ(root.value("schema_version").toString(), MANIFEST_SCHEMA);

    const QJsonObject scoreObject = root.value("score").toObject();
    EXPECT_EQ(scoreObject.value("base_path").toString(), "song");
    EXPECT_EQ(scoreObject.value("mei").toObject().value("path").toString(), "song.mei");

    const QJsonObject repeatMetadata = root.value("repeat_metadata").toObject();
    EXPECT_EQ(repeatMetadata.value("has_repeats").toBool(), expectation.hasRepeats);
    EXPECT_EQ(repeatMetadata.value("has_multiple_endings").toBool(), expectation.hasMultipleEndings);
    EXPECT_EQ(repeatMetadata.value("final_ending_number").toInt(), expectation.finalEndingNumber);

    const QJsonArray canonical = root.value("canonical_events").toArray();
    EXPECT_EQ(canonical.size(), expectation.canonicalEvents);
    EXPECT_EQ(countCanonicalEvents(canonical, "event_type", "written_grace"), expectation.writtenGraceEvents);
    EXPECT_EQ(countCanonicalEvents(canonical, "alias_reason", "same_onset"), expectation.sameOnsetAliases);
    EXPECT_EQ(countCanonicalEvents(canonical, "alias_reason", "tie_continuation"), expectation.tieAliases);
    const QJsonArray ornamentDefinitions = root.value("ornament_definitions").toArray();
    EXPECT_EQ(ornamentDefinitions.size(), expectation.ornamentDefinitions);

    std::set<QString> canonicalIds;
    std::map<QString, QJsonObject> canonicalById;
    for (const QJsonValue& value : canonical) {
        const QJsonObject event = value.toObject();
        const QString eventId = event.value("score_event_id").toString();
        EXPECT_TRUE(canonicalIds.insert(eventId).second);
        canonicalById.emplace(eventId, event);
    }
    for (const auto& [eventId, event] : canonicalById) {
        if (event.value("alias_reason").toString() != "same_onset") {
            continue;
        }
        const QString targetId = event.value("alias_of").toString();
        ASSERT_TRUE(canonicalById.count(targetId));
        EXPECT_EQ(
            event.value("simultaneous_group_id").toString(),
            canonicalById[targetId].value("simultaneous_group_id").toString())
            << eventId.toStdString();
    }
    std::set<QString> ornamentDefinitionIds;
    for (const QJsonValue& value : ornamentDefinitions) {
        EXPECT_TRUE(ornamentDefinitionIds.insert(
            value.toObject().value("ornament_definition_id").toString()).second);
    }

    const QJsonArray traversals = root.value("traversals").toArray();
    const QJsonArray files = root.value("files").toArray();
    ASSERT_EQ(traversals.size(), static_cast<int>(expectation.files.size()));
    ASSERT_EQ(files.size(), static_cast<int>(expectation.files.size()));

    for (size_t index = 0; index < expectation.files.size(); ++index) {
        const FileExpectation& expectedFile = expectation.files[index];
        const QJsonObject traversal = traversals.at(static_cast<int>(index)).toObject();
        const QJsonObject file = files.at(static_cast<int>(index)).toObject();
        EXPECT_EQ(traversal.value("traversal_id").toString(), expectedFile.kind);
        EXPECT_EQ(traversal.value("variant_kind").toString(), expectedFile.kind);
        EXPECT_FALSE(traversal.value("segments").toArray().isEmpty());
        EXPECT_EQ(file.value("kind").toString(), expectedFile.kind);
        EXPECT_EQ(file.value("traversal_id").toString(), expectedFile.kind);
        EXPECT_EQ(file.value("expand_repeats").toBool(), std::string(expectedFile.kind) == "repeats");
        EXPECT_EQ(file.value("audible_events").toArray().size(), expectedFile.audibleEvents);
        EXPECT_EQ(file.value("visual_occurrences").toArray().size(), expectedFile.visualOccurrences);
        EXPECT_EQ(file.value("ornament_occurrences").toArray().size(), expectedFile.ornamentOccurrences);

        std::set<QString> expectedAliasIds;
        for (const QJsonValue& value : canonical) {
            const QJsonObject event = value.toObject();
            if (event.value("alias_of").isNull()) {
                continue;
            }
            for (const QJsonValue& membership : event.value("variant_membership").toArray()) {
                if (membership.toString() == expectedFile.kind) {
                    expectedAliasIds.insert(event.value("score_event_id").toString());
                }
            }
        }
        verifyOccurrenceLinkage(file, canonicalIds, ornamentDefinitionIds, expectedAliasIds);

        const QString relativePath = file.value("path").toString();
        EXPECT_FALSE(relativePath.isEmpty());
        const QByteArray firstMidi = readFile(firstDirectory.filePath(relativePath));
        const QByteArray secondMidi = readFile(secondDirectory.filePath(relativePath));
        EXPECT_FALSE(firstMidi.isEmpty());
        EXPECT_EQ(firstMidi, secondMidi);
    }
}

INSTANTIATE_TEST_SUITE_P(
    FixtureMatrix,
    Project_PianomaniaExportTest,
    ::testing::Values(
        ExportCase {
            "simple",
            "src/importexport/midi/tests/midiexport_data/pianomania_trill_ordinary_notes.mscx",
            false, false, 0, 2, 0, 0, 0, 0,
            { { "base", 2, 2, 0 } }
        },
        ExportCase {
            "simple_repeat",
            "src/importexport/midi/tests/midiexport_data/testInitialKeySigThenRepeatToMeas2.mscx",
            true, false, 0, 8, 0, 0, 0, 0,
            { { "base", 8, 8, 0 }, { "repeats", 12, 12, 0 } }
        },
        ExportCase {
            "multiple_endings",
            "src/engraving/tests/playback/playbackmodel_data/repeat_with_2_voltas/repeat_with_2_voltas.mscx",
            true, true, 2, 24, 0, 0, 0, 0,
            { { "base", 24, 24, 0 }, { "no-repeats", 20, 20, 0 }, { "repeats", 28, 28, 0 } }
        },
        ExportCase {
            "written_grace",
            "src/importexport/mei/tests/data/pianomania-grace-same-pitch-index.mscx",
            false, false, 0, 4, 2, 0, 0, 2,
            { { "base", 6, 4, 2 } }
        },
        ExportCase {
            "ornament",
            "src/importexport/midi/tests/midiexport_data/pianomania_trill_basic_line.mscx",
            false, false, 0, 1, 0, 0, 0, 23,
            { { "base", 24, 1, 23 } }
        },
        ExportCase {
            "ties",
            "src/importexport/mei/tests/data/tie-01.mscx",
            false, false, 0, 22, 0, 0, 14, 0,
            { { "base", 8, 22, 0 } }
        },
        ExportCase {
            "unison_same_onset_alias",
            "src/importexport/midi/tests/midiexport_data/pianomania_same_start_unison.mscx",
            false, false, 0, 2, 0, 1, 0, 0,
            { { "base", 1, 2, 0 } }
        }
        ),
    [](const ::testing::TestParamInfo<ExportCase>& info) {
        return info.param.name;
    }
    );
