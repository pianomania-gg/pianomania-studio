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
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "engraving/dom/masterscore.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"
#include "engraving/pm/pmlayout.h"
#include "engraving/pm/pmprettify.h"
#include "engraving/rw/compat/pianomaniaheldnotepitchcurvemigration.h"
#include "engraving/tests/utils/scorerw.h"
#include "project/internal/pianomaniaexport.h"
#include "project/internal/pianomaniacompatibility.generated.h"
#include "engraving/types/types.h"

namespace {
constexpr const char* MANIFEST_SCHEMA =
    mu::project::pianomania::compatibility::EXPORTER_PROVENANCE_SCHEMA;

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

struct DecodedMidiChannelEvent {
    int tick = 0;
    int ordinal = 0;
    uint8_t status = 0;
    uint8_t data1 = 0;
    uint8_t data2 = 0;
};

uint32_t readBigEndian32(const QByteArray& bytes, int offset)
{
    return (static_cast<uint8_t>(bytes[offset]) << 24) | (static_cast<uint8_t>(bytes[offset + 1]) << 16)
           | (static_cast<uint8_t>(bytes[offset + 2]) << 8) | static_cast<uint8_t>(bytes[offset + 3]);
}

uint32_t readVariableLength(const QByteArray& bytes, int* offset, int end)
{
    uint32_t value = 0;
    for (int count = 0; count < 4 && *offset < end; ++count) {
        const uint8_t byte = static_cast<uint8_t>(bytes[(*offset)++]);
        value = (value << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    return value;
}

std::vector<DecodedMidiChannelEvent> decodeMidiChannelEvents(const QByteArray& bytes)
{
    std::vector<DecodedMidiChannelEvent> result;
    if (bytes.size() < 14 || bytes.mid(0, 4) != "MThd") {
        return result;
    }
    int offset = 8 + static_cast<int>(readBigEndian32(bytes, 4));
    int ordinal = 0;
    while (offset + 8 <= bytes.size()) {
        const QByteArray chunkType = bytes.mid(offset, 4);
        const int chunkLength = static_cast<int>(readBigEndian32(bytes, offset + 4));
        offset += 8;
        const int end = std::min(offset + chunkLength, static_cast<int>(bytes.size()));
        if (chunkType != "MTrk") {
            offset = end;
            continue;
        }

        int tick = 0;
        uint8_t runningStatus = 0;
        while (offset < end) {
            tick += static_cast<int>(readVariableLength(bytes, &offset, end));
            if (offset >= end) {
                break;
            }
            uint8_t status = static_cast<uint8_t>(bytes[offset]);
            if ((status & 0x80) != 0) {
                ++offset;
                if (status < 0xF0) {
                    runningStatus = status;
                }
            } else {
                status = runningStatus;
            }
            if (status == 0xFF) {
                if (offset >= end) {
                    break;
                }
                ++offset;
                const int length = static_cast<int>(readVariableLength(bytes, &offset, end));
                offset = std::min(offset + length, end);
                runningStatus = 0;
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {
                const int length = static_cast<int>(readVariableLength(bytes, &offset, end));
                offset = std::min(offset + length, end);
                runningStatus = 0;
                continue;
            }
            const int dataLength = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 1 : 2;
            if (status < 0x80 || status >= 0xF0 || offset + dataLength > end) {
                break;
            }
            DecodedMidiChannelEvent event;
            event.tick = tick;
            event.ordinal = ordinal++;
            event.status = status;
            event.data1 = static_cast<uint8_t>(bytes[offset++]);
            event.data2 = dataLength == 2 ? static_cast<uint8_t>(bytes[offset++]) : 0;
            result.push_back(event);
        }
        offset = end;
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.tick == right.tick ? left.ordinal < right.ordinal : left.tick < right.tick;
    });
    return result;
}

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

std::vector<mu::engraving::Note*> notesWithPitch(mu::engraving::Score* score, int pitch)
{
    std::vector<mu::engraving::Note*> notes;
    score->scanElements([&](mu::engraving::EngravingItem* item) {
        if (item && item->isNote() && mu::engraving::toNote(item)->pitch() == pitch) {
            notes.push_back(mu::engraving::toNote(item));
        }
    });
    return notes;
}

QString writeModifiedFixture(const QByteArray& source, const QByteArray& from, const QByteArray& to,
                             QTemporaryDir* directory)
{
    QByteArray modified = source;
    modified.replace(from, to);
    const QString path = directory->filePath("modified.mscx");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(modified) != modified.size()) {
        return {};
    }
    return path;
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

std::set<QString> verifyFinalPublicIdentityGraph(const QByteArray& mei, const QJsonObject& manifest)
{
    EXPECT_FALSE(mei.isEmpty());
    EXPECT_FALSE(mei.contains("xml:id=\"mscore-"));
    EXPECT_FALSE(mei.contains("#mscore-"));
    EXPECT_FALSE(QJsonDocument(manifest).toJson(QJsonDocument::Compact).contains("mscore-"));

    const QString text = QString::fromUtf8(mei);
    const QRegularExpression declarationPattern(QStringLiteral("xml:id=\\\"(pm-score-[0-9]{6})\\\""));
    const QRegularExpression referencePattern(QStringLiteral("#(pm-score-[0-9]{6})"));
    const QRegularExpression pianomaniaReferencePattern(
        QStringLiteral("(?:pm:covered-id|pm:covered-ids|pm:coveredUuids|pm:terminal-ids|pm:post-terminal-ids)=\\\"([^\\\"]*)\\\""));
    const QRegularExpression publicIdentityPattern(QStringLiteral("^pm-score-[0-9]{6}$"));

    std::set<QString> declared;
    QRegularExpressionMatchIterator declarations = declarationPattern.globalMatch(text);
    int ordinal = 0;
    while (declarations.hasNext()) {
        const QString identity = declarations.next().captured(1);
        ++ordinal;
        EXPECT_EQ(identity, QStringLiteral("pm-score-%1").arg(ordinal, 6, 10, QLatin1Char('0')));
        EXPECT_TRUE(declared.insert(identity).second);
    }
    EXPECT_FALSE(declared.empty());

    QRegularExpressionMatchIterator references = referencePattern.globalMatch(text);
    while (references.hasNext()) {
        EXPECT_TRUE(declared.count(references.next().captured(1)));
    }
    QRegularExpressionMatchIterator pianomaniaReferences = pianomaniaReferencePattern.globalMatch(text);
    while (pianomaniaReferences.hasNext()) {
        for (const QString& identity : pianomaniaReferences.next().captured(1).split(' ', Qt::SkipEmptyParts)) {
            EXPECT_TRUE(declared.count(identity));
        }
    }

    const QJsonArray canonical = manifest.value("canonical_events").toArray();
    for (const QJsonValue& value : canonical) {
        const QJsonObject event = value.toObject();
        EXPECT_TRUE(declared.count(event.value("score_event_id").toString()));
        EXPECT_TRUE(declared.count(event.value("measure_id").toString()));
        EXPECT_TRUE(publicIdentityPattern.match(event.value("chord_id").toString()).hasMatch());
        if (!event.value("alias_of").isNull()) {
            EXPECT_TRUE(declared.count(event.value("alias_of").toString()));
        }
    }
    for (const QJsonValue& value : manifest.value("ornament_definitions").toArray()) {
        EXPECT_TRUE(declared.count(value.toObject().value("target_score_event_id").toString()));
    }
    for (const QJsonValue& fileValue : manifest.value("files").toArray()) {
        const QJsonObject file = fileValue.toObject();
        for (const QJsonValue& occurrenceValue : file.value("visual_occurrences").toArray()) {
            EXPECT_TRUE(declared.count(occurrenceValue.toObject().value("score_event_id").toString()));
        }
        for (const QJsonValue& audibleValue : file.value("audible_events").toArray()) {
            for (const QJsonValue& aliasValue : audibleValue.toObject().value("alias_ids").toArray()) {
                EXPECT_TRUE(declared.count(aliasValue.toString()));
            }
        }
    }
    return declared;
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
        const QJsonObject noteOnLocator = event.value("note_on_locator").toObject();
        const QJsonObject noteOffLocator = event.value("note_off_locator").toObject();
        verifyLocator(noteOnLocator);
        verifyLocator(noteOffLocator);
        const int heardMidiKey = event.value("heard_midi_key").toInt();
        EXPECT_EQ(noteOnLocator.value("data").toArray().at(0).toInt(), heardMidiKey);
        EXPECT_EQ(noteOffLocator.value("data").toArray().at(0).toInt(), heardMidiKey);

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

void verifyVisualAnchorsPrecedeOrnaments(const QJsonObject& file, const QJsonArray& ornamentDefinitions)
{
    std::map<QString, QJsonObject> audibleById;
    for (const QJsonValue& value : file.value("audible_events").toArray()) {
        const QJsonObject event = value.toObject();
        audibleById.emplace(event.value("audible_event_id").toString(), event);
    }

    std::map<QString, int> visualAnchorTicks;
    for (const QJsonValue& value : file.value("visual_occurrences").toArray()) {
        const QJsonObject occurrence = value.toObject();
        const auto audible = audibleById.find(occurrence.value("audible_event_id").toString());
        ASSERT_NE(audible, audibleById.cend());
        EXPECT_EQ(audible->second.value("event_type").toString(), "written");
        EXPECT_EQ(audible->second.value("note_off_velocity").toInt(), 64);
        visualAnchorTicks.emplace(
            occurrence.value("score_event_id").toString(),
            audible->second.value("note_on_locator").toObject().value("absolute_tick").toInt());
    }

    std::map<QString, QJsonObject> definitionById;
    for (const QJsonValue& value : ornamentDefinitions) {
        const QJsonObject definition = value.toObject();
        definitionById.emplace(definition.value("ornament_definition_id").toString(), definition);
    }

    for (const QJsonValue& value : file.value("ornament_occurrences").toArray()) {
        const QJsonObject occurrence = value.toObject();
        const auto definition = definitionById.find(occurrence.value("ornament_definition_id").toString());
        ASSERT_NE(definition, definitionById.cend());
        const auto anchorTick = visualAnchorTicks.find(definition->second.value("target_score_event_id").toString());
        ASSERT_NE(anchorTick, visualAnchorTicks.cend());
        const auto audible = audibleById.find(occurrence.value("audible_event_id").toString());
        ASSERT_NE(audible, audibleById.cend());

        const QJsonObject noteOnLocator = audible->second.value("note_on_locator").toObject();
        const QJsonObject noteOffLocator = audible->second.value("note_off_locator").toObject();
        const int heardMidiKey = audible->second.value("heard_midi_key").toInt();
        EXPECT_EQ(audible->second.value("event_type").toString(), "nonvisual_ornament");
        EXPECT_EQ(audible->second.value("note_off_velocity").toInt(), 127);
        EXPECT_EQ(definition->second.value("heard_midi_key").toInt(), heardMidiKey);
        EXPECT_EQ(noteOnLocator.value("data").toArray().at(0).toInt(), heardMidiKey);
        EXPECT_EQ(noteOffLocator.value("data").toArray().at(0).toInt(), heardMidiKey);
        EXPECT_LT(anchorTick->second, noteOnLocator.value("absolute_tick").toInt());
    }
}

std::vector<std::pair<bool, bool> > notePlaybackState(mu::engraving::Score* score)
{
    std::vector<std::pair<bool, bool> > result;
    score->scanElements([&result](mu::engraving::EngravingItem* item) {
        if (!item || !item->isNote()) {
            return;
        }
        const auto* note = mu::engraving::toNote(item);
        result.push_back({ note->visible(), note->play() });
    });
    return result;
}
}

class Project_PianomaniaExportTest : public ::testing::TestWithParam<ExportCase>
{
};

TEST(Project_PianomaniaCoordinatedExportTest, LayoutAndPrettifyAreReflectedInCanonicalMei)
{
    ScopedMScoreTestMode productionScoreSerialization(false);

    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/project/tests/data/pianomania-coordinated-export.mscx";
    std::unique_ptr<mu::engraving::MasterScore> autoLayoutOnly(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    std::unique_ptr<mu::engraving::MasterScore> coordinated(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(autoLayoutOnly, nullptr);
    ASSERT_NE(coordinated, nullptr);

    autoLayoutOnly->startCmd(muse::TranslatableString::untranslatable("Pianomania coordinated export test"));
    mu::engraving::pm::applyPianomaniaAutoLayout(autoLayoutOnly.get());
    autoLayoutOnly->endCmd(false);
    coordinated->startCmd(muse::TranslatableString::untranslatable("Pianomania coordinated export test"));
    mu::engraving::pm::applyPianomaniaAutoLayout(coordinated.get());
    coordinated->endCmd(false);
    coordinated->startCmd(muse::TranslatableString::untranslatable("Pianomania coordinated export test"));
    const mu::engraving::pm::PmPrettifyResult prettifyResult
        = mu::engraving::pm::applyPianomaniaPrettify(coordinated.get());
    coordinated->endCmd(!prettifyResult.changed || prettifyResult.structuralAssignmentChanged);
    ASSERT_TRUE(prettifyResult.changed);
    coordinated->setLayoutAll();
    coordinated->doLayout();

    QTemporaryDir autoLayoutDirectory;
    QTemporaryDir coordinatedDirectory;
    ASSERT_TRUE(autoLayoutDirectory.isValid());
    ASSERT_TRUE(coordinatedDirectory.isValid());

    const auto exportScore = [&](mu::engraving::MasterScore* score, const QTemporaryDir& directory) {
        return mu::project::pianomania::exportPianomaniaBundle(
            score,
            muse::io::path_t(fixturePath),
            muse::io::path_t(directory.filePath("song")),
            muse::io::path_t(directory.filePath("song.mei")),
            false);
    };

    const muse::RetVal<mu::project::pianomania::PianomaniaExportResult> autoLayoutResult
        = exportScore(autoLayoutOnly.get(), autoLayoutDirectory);
    ASSERT_TRUE(autoLayoutResult.ret) << autoLayoutResult.ret.toString();
    const muse::RetVal<mu::project::pianomania::PianomaniaExportResult> coordinatedResult
        = exportScore(coordinated.get(), coordinatedDirectory);
    ASSERT_TRUE(coordinatedResult.ret) << coordinatedResult.ret.toString();

    const QByteArray autoLayoutMei = readFile(autoLayoutDirectory.filePath("song.mei"));
    const QByteArray coordinatedMei = readFile(coordinatedDirectory.filePath("song.mei"));
    ASSERT_FALSE(coordinatedMei.isEmpty());
    EXPECT_NE(coordinatedMei, autoLayoutMei)
        << "Prettify geometry must be serialized into the canonical MEI";
    EXPECT_TRUE(coordinatedMei.contains("pm:spatium=\"0.070000\""));
    EXPECT_FALSE(coordinatedMei.contains("<pgHead"));
    EXPECT_TRUE(coordinatedMei.contains("Pianomania Coordinated Export"))
        << "Removing the visual title frame must not remove score title metadata";
}

TEST(Project_PianomaniaCoordinatedExportTest, CanonicalPlaybackNormalizationIsExportScoped)
{
    ScopedMScoreTestMode productionScoreSerialization(false);

    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/project/tests/data/pianomania-playback-authority.mscx";
    const QByteArray sourceBytes = readFile(fixturePath.toQString());
    std::unique_ptr<mu::engraving::MasterScore> score(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);

    const std::vector<std::pair<bool, bool> > before = notePlaybackState(score.get());
    ASSERT_EQ(before.size(), 3);
    EXPECT_EQ(std::count(before.cbegin(), before.cend(), std::make_pair(true, false)), 1);
    EXPECT_EQ(std::count(before.cbegin(), before.cend(), std::make_pair(true, true)), 1);
    EXPECT_EQ(std::count(before.cbegin(), before.cend(), std::make_pair(false, true)), 1);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const muse::RetVal<mu::project::pianomania::PianomaniaExportResult> result
        = mu::project::pianomania::exportPianomaniaBundle(
            score.get(), muse::io::path_t(fixturePath),
            muse::io::path_t(directory.filePath("song")),
            muse::io::path_t(directory.filePath("song.mei")), false);
    ASSERT_TRUE(result.ret) << result.ret.toString();

    EXPECT_EQ(notePlaybackState(score.get()), before)
        << "Export must restore authored note visibility/playback state";
    EXPECT_EQ(readFile(fixturePath.toQString()), sourceBytes)
        << "Export must not rewrite the canonical source score";
}

TEST(Project_PianomaniaCoordinatedExportTest, TiedContinuationsDoNotRequireNewPlaybackAttacks)
{
    ScopedMScoreTestMode productionScoreSerialization(false);

    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/importexport/midi/tests/midiexport_data/"
                                       "pianomania_cross_voice_tie.mscx";
    std::unique_ptr<mu::engraving::MasterScore> score(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);

    std::vector<mu::engraving::Note*> continuations;
    score->scanElements([&continuations](mu::engraving::EngravingItem* item) {
        if (!item || !item->isNote()) {
            return;
        }
        auto* note = mu::engraving::toNote(item);
        if (note->tieBack()) {
            note->playEvents().clear();
            continuations.push_back(note);
        }
    });
    ASSERT_EQ(continuations.size(), 1u);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const muse::RetVal<mu::project::pianomania::PianomaniaExportResult> result
        = mu::project::pianomania::exportPianomaniaBundle(
            score.get(), muse::io::path_t(fixturePath),
            muse::io::path_t(directory.filePath("song")),
            muse::io::path_t(directory.filePath("song.mei")), false);

    ASSERT_TRUE(result.ret) << result.ret.toString();
    EXPECT_EQ(result.val.canonicalEvents.size(), 2u);
    ASSERT_EQ(result.val.files.size(), 1u);
    EXPECT_EQ(result.val.files[0].audibleEvents.size(), 1u);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(result.val.manifestJson), &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError);
    EXPECT_EQ(
        countCanonicalEvents(document.object().value("canonical_events").toArray(),
                             "alias_reason", "tie_continuation"),
        1);
}

TEST(Project_PianomaniaCoordinatedExportTest, PublicArtifactsAreIndependentOfInternalMuseScoreIdentities)
{
    ScopedMScoreTestMode productionScoreSerialization(false);

    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/importexport/midi/tests/midiexport_data/"
                                       "pianomania_cross_voice_tie.mscx";
    std::unique_ptr<mu::engraving::MasterScore> firstScore(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    std::unique_ptr<mu::engraving::MasterScore> secondScore(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(firstScore, nullptr);
    ASSERT_NE(secondScore, nullptr);

    secondScore->assignNewEID();
    secondScore->scanElements([](mu::engraving::EngravingItem* item) {
        if (item) {
            item->assignNewEID();
        }
    });

    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    ASSERT_TRUE(firstDirectory.isValid());
    ASSERT_TRUE(secondDirectory.isValid());
    const auto exportScore = [&](mu::engraving::Score* score, const QTemporaryDir& directory) {
        return mu::project::pianomania::exportPianomaniaBundle(
            score, muse::io::path_t(fixturePath),
            muse::io::path_t(directory.filePath("song")),
            muse::io::path_t(directory.filePath("song.mei")), false);
    };

    const auto first = exportScore(firstScore.get(), firstDirectory);
    const auto second = exportScore(secondScore.get(), secondDirectory);
    ASSERT_TRUE(first.ret) << first.ret.toString();
    ASSERT_TRUE(second.ret) << second.ret.toString();
    EXPECT_EQ(first.val.manifestJson, second.val.manifestJson);
    EXPECT_EQ(readFile(firstDirectory.filePath("song.mei")), readFile(secondDirectory.filePath("song.mei")));
    EXPECT_EQ(readFile(firstDirectory.filePath("song.mid")), readFile(secondDirectory.filePath("song.mid")));
}

TEST(Project_PianomaniaCoordinatedExportTest, HeldNotePitchCurveExportsDeterministicCanonicalMei)
{
    ScopedMScoreTestMode productionScoreSerialization(false);
    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/importexport/midi/tests/midiexport_data/pianomania_trill_ordinary_notes.mscx";
    std::unique_ptr<mu::engraving::MasterScore> score(mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);

    mu::engraving::Note* heldNote = nullptr;
    score->scanElements([&heldNote](mu::engraving::EngravingItem* item) {
        if (!heldNote && item && item->isNote()) {
            heldNote = mu::engraving::toNote(item);
        }
    });
    ASSERT_NE(heldNote, nullptr);
    heldNote->setPianomaniaHeldNote(true);
    const int durationTicks = heldNote->pianomaniaHeldNoteDurationTicks();
    ASSERT_GT(durationTicks, 1);
    heldNote->setPianomaniaHeldNotePitchCurve({
        { 0, 0, 0 }, { durationTicks / 2, -200, -1200 }, { durationTicks, -1200, 0 }
    });

    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    ASSERT_TRUE(firstDirectory.isValid());
    ASSERT_TRUE(secondDirectory.isValid());
    const auto exportTo = [&](const QTemporaryDir& directory) {
        return mu::project::pianomania::exportPianomaniaBundle(
            score.get(), muse::io::path_t(fixturePath), muse::io::path_t(directory.filePath("song")),
            muse::io::path_t(directory.filePath("song.mei")), false);
    };

    const auto first = exportTo(firstDirectory);
    const auto second = exportTo(secondDirectory);
    ASSERT_TRUE(first.ret) << first.ret.toString();
    ASSERT_TRUE(second.ret) << second.ret.toString();
    const QByteArray firstMei = readFile(firstDirectory.filePath("song.mei"));
    EXPECT_EQ(firstMei, readFile(secondDirectory.filePath("song.mei")));
    const QByteArray expectedCurve = "pm:heldPitchCurveV3=\"0:0:0;"
                                     + QByteArray::number(durationTicks / 2) + ":-200:-1200;"
                                     + QByteArray::number(durationTicks) + ":-1200:0\"";
    EXPECT_EQ(firstMei.count(expectedCurve), 1);
    EXPECT_EQ(firstMei.count("pm:heldPitchCurveV2="), 0);

    const QByteArray firstMidi = readFile(firstDirectory.filePath("song.mid"));
    EXPECT_EQ(firstMidi, readFile(secondDirectory.filePath("song.mid")));
    const std::vector<DecodedMidiChannelEvent> midiEvents = decodeMidiChannelEvents(firstMidi);
    std::vector<DecodedMidiChannelEvent> pitchBends;
    std::copy_if(midiEvents.begin(), midiEvents.end(), std::back_inserter(pitchBends), [](const auto& event) {
        return (event.status & 0xF0) == 0xE0;
    });
    ASSERT_GT(pitchBends.size(), 10u);
    EXPECT_EQ(pitchBends.front().tick, heldNote->chord()->tick().ticks());
    EXPECT_EQ(pitchBends.front().data1 | (pitchBends.front().data2 << 7), 8192);
    EXPECT_EQ(pitchBends.back().tick, heldNote->chord()->tick().ticks() + durationTicks);
    EXPECT_EQ(pitchBends.back().data1 | (pitchBends.back().data2 << 7), 8192);
    EXPECT_TRUE(std::any_of(pitchBends.begin(), pitchBends.end(), [&](const auto& event) {
        return event.tick == heldNote->chord()->tick().ticks() + durationTicks / 2
               && std::abs((event.data1 | (event.data2 << 7)) - 6827) <= 1;
    }));
    std::vector<int> terminalPitchValues;
    for (const auto& event : pitchBends) {
        if (event.tick == heldNote->chord()->tick().ticks() + durationTicks) {
            terminalPitchValues.push_back(event.data1 | (event.data2 << 7));
        }
    }
    ASSERT_GE(terminalPitchValues.size(), 2u);
    EXPECT_EQ(terminalPitchValues[0], 0);
    EXPECT_EQ(terminalPitchValues[1], 8192);

    const uint8_t pitchChannel = pitchBends.front().status & 0x0F;
    EXPECT_TRUE(std::any_of(midiEvents.begin(), midiEvents.end(), [&](const auto& event) {
        return event.tick == pitchBends.front().tick
               && event.status == static_cast<uint8_t>(0xB0 | pitchChannel) && event.data1 == 6 && event.data2 == 12;
    }));
    EXPECT_TRUE(std::any_of(midiEvents.begin(), midiEvents.end(), [&](const auto& event) {
        return event.tick == heldNote->chord()->tick().ticks() + durationTicks
               && (event.status & 0xF0) == 0x80 && event.data1 == heldNote->pitch();
    }));
}

TEST(Project_PianomaniaCoordinatedExportTest, HeldNotePitchCurveSensitivityIncludesFractionalTickExtrema)
{
    ScopedMScoreTestMode productionScoreSerialization(false);
    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/importexport/midi/tests/midiexport_data/pianomania_trill_ordinary_notes.mscx";
    std::unique_ptr<mu::engraving::MasterScore> score(mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);

    mu::engraving::Note* heldNote = nullptr;
    score->scanElements([&heldNote](mu::engraving::EngravingItem* item) {
        if (!heldNote && item && item->isNote()) {
            heldNote = mu::engraving::toNote(item);
        }
    });
    ASSERT_NE(heldNote, nullptr);
    heldNote->setPianomaniaHeldNote(true);
    const int durationTicks = heldNote->pianomaniaHeldNoteDurationTicks();
    ASSERT_GT(durationTicks, 3);

    // The one-tick Hermite segment from tick 1 to 2 reaches 100.25 cents at
    // tick 1.5 even though both integer-tick endpoints are only 99 cents.
    heldNote->setPianomaniaHeldNotePitchCurve({
        { 0, 0, 0 }, { 1, 99, 2400 }, { 2, 99, -2400 }, { 3, 0, 0 }, { durationTicks, 0, 0 }
    });

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto exported = mu::project::pianomania::exportPianomaniaBundle(
        score.get(), muse::io::path_t(fixturePath), muse::io::path_t(directory.filePath("song")),
        muse::io::path_t(directory.filePath("song.mei")), false);
    ASSERT_TRUE(exported.ret) << exported.ret.toString();

    const std::vector<DecodedMidiChannelEvent> midiEvents
        = decodeMidiChannelEvents(readFile(directory.filePath("song.mid")));
    std::ostringstream dataEntryValues;
    for (const auto& event : midiEvents) {
        if ((event.status & 0xF0) == 0xB0 && event.data1 == 6) {
            dataEntryValues << event.tick << ':' << static_cast<int>(event.data2) << ' ';
        }
    }
    EXPECT_TRUE(std::any_of(midiEvents.begin(), midiEvents.end(), [](const auto& event) {
        return (event.status & 0xF0) == 0xB0 && event.data1 == 6 && event.data2 == 2;
    })) << dataEntryValues.str();
}

TEST(Project_PianomaniaCoordinatedExportTest, HeldNotePitchCurveV2MigratesAfterRead460ConnectsTies)
{
    ScopedMScoreTestMode productionScoreSerialization(false);
    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/project/tests/data/pianomania-held-pitch-curve-v2-470.mscx";
    std::unique_ptr<mu::engraving::MasterScore> score(mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);

    const mu::engraving::PianomaniaHeldNotePitchCurve threeWholeExpected {
        { 0, 0, 0 }, { 3840, 30, 11 }, { 4032, 430, 0 }, { 4320, 30, -25 }, { 5760, 0, 0 }
    };
    const std::vector<mu::engraving::Note*> threeWholeChain = notesWithPitch(score.get(), 60);
    ASSERT_EQ(threeWholeChain.size(), 3u);
    for (const mu::engraving::Note* note : threeWholeChain) {
        EXPECT_EQ(note->pianomaniaHeldNoteDurationTicks(), 5760);
        EXPECT_EQ(note->pianomaniaHeldNotePitchCurve(), threeWholeExpected);
    }

    const mu::engraving::PianomaniaHeldNotePitchCurve twoHalfExpected {
        { 0, 0, 0 }, { 256, 0, 0 }, { 960, -1210, -818 }, { 1664, -2400, 0 }, { 1920, -2400, 0 }
    };
    const std::vector<mu::engraving::Note*> twoHalfChain = notesWithPitch(score.get(), 64);
    ASSERT_EQ(twoHalfChain.size(), 2u);
    for (const mu::engraving::Note* note : twoHalfChain) {
        EXPECT_EQ(note->pianomaniaHeldNoteDurationTicks(), 1920);
        EXPECT_EQ(note->pianomaniaHeldNotePitchCurve(), twoHalfExpected);
    }

    const std::vector<mu::engraving::Note*> singleNotes = notesWithPitch(score.get(), 67);
    ASSERT_EQ(singleNotes.size(), 1u);
    EXPECT_EQ(singleNotes.front()->pianomaniaHeldNoteDurationTicks(), 1920);
    EXPECT_EQ(singleNotes.front()->pianomaniaHeldNotePitchCurve(),
              (mu::engraving::PianomaniaHeldNotePitchCurve { { 0, 0, 0 }, { 960, 200, 25 }, { 1920, 0, 0 } }));

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto exported = mu::project::pianomania::exportPianomaniaBundle(
        score.get(), muse::io::path_t(fixturePath), muse::io::path_t(directory.filePath("song")),
        muse::io::path_t(directory.filePath("song.mei")), false);
    ASSERT_TRUE(exported.ret) << exported.ret.toString();
    const QByteArray mei = readFile(directory.filePath("song.mei"));
    EXPECT_EQ(mei.count("pm:heldPitchCurveV3="), 6);
    EXPECT_EQ(mei.count("pm:heldPitchCurveV2="), 0);

    const QString savedPath = directory.filePath("migrated.mscx");
    ASSERT_TRUE(mu::engraving::ScoreRW::saveScore(score.get(), muse::String::fromUtf8(savedPath.toUtf8().constData())));
    const QByteArray saved = readFile(savedPath);
    EXPECT_EQ(saved.count("pianomaniaHeldNotePitchCurve version=\"2\""), 0);
    EXPECT_EQ(saved.count("pianomaniaHeldNotePitchCurve version=\"3\""), 6);

    std::unique_ptr<mu::engraving::MasterScore> reopened(
        mu::engraving::ScoreRW::readScore(muse::String::fromUtf8(savedPath.toUtf8().constData()), true));
    ASSERT_NE(reopened, nullptr);
    ASSERT_EQ(notesWithPitch(reopened.get(), 60).size(), 3u);
    EXPECT_EQ(notesWithPitch(reopened.get(), 60).front()->pianomaniaHeldNotePitchCurve(), threeWholeExpected);
}

TEST(Project_PianomaniaCoordinatedExportTest, HeldNotePitchCurveV2RejectsMalformedAndInconsistentTieChains)
{
    const QString fixturePath = QString::fromUtf8(project_test_DATA_ROOT)
                                + "/src/project/tests/data/pianomania-held-pitch-curve-v2-470.mscx";
    const QByteArray source = readFile(fixturePath);
    ASSERT_FALSE(source.isEmpty());

    QTemporaryDir inconsistentDirectory;
    ASSERT_TRUE(inconsistentDirectory.isValid());
    QByteArray inconsistent = source;
    const qsizetype curveStart = inconsistent.indexOf("<pianomaniaHeldNotePitchCurve version=\"2\">");
    const qsizetype curveEnd = inconsistent.indexOf("</pianomaniaHeldNotePitchCurve>", curveStart);
    ASSERT_GE(curveStart, 0);
    ASSERT_GE(curveEnd, 0);
    const QByteArray curveClosingTag = "</pianomaniaHeldNotePitchCurve>";
    inconsistent.remove(curveStart, curveEnd + curveClosingTag.size() - curveStart);
    const QString inconsistentPath = writeModifiedFixture(inconsistent, "unchanged-sentinel", "unchanged-sentinel",
                                                          &inconsistentDirectory);
    ASSERT_FALSE(inconsistentPath.isEmpty());
    EXPECT_EQ(mu::engraving::ScoreRW::readScore(
                  muse::String::fromUtf8(inconsistentPath.toUtf8().constData()), true), nullptr);

    QTemporaryDir differingDirectory;
    ASSERT_TRUE(differingDirectory.isValid());
    QByteArray differing = source;
    const QByteArray originalPoint = "time=\"42000\" pitch=\"430\"";
    const qsizetype firstPitch = differing.indexOf(originalPoint);
    ASSERT_GE(firstPitch, 0);
    differing.replace(firstPitch, originalPoint.size(), "time=\"42000\" pitch=\"440\"");
    const QString differingPath = writeModifiedFixture(differing, "unchanged-sentinel", "unchanged-sentinel",
                                                       &differingDirectory);
    ASSERT_FALSE(differingPath.isEmpty());
    EXPECT_EQ(mu::engraving::ScoreRW::readScore(
                  muse::String::fromUtf8(differingPath.toUtf8().constData()), true), nullptr);

    QTemporaryDir malformedDirectory;
    ASSERT_TRUE(malformedDirectory.isValid());
    const QString malformedPath = writeModifiedFixture(source, "time=\"42000\"", "time=\"70000\"", &malformedDirectory);
    ASSERT_FALSE(malformedPath.isEmpty());
    EXPECT_EQ(mu::engraving::ScoreRW::readScore(
                  muse::String::fromUtf8(malformedPath.toUtf8().constData()), true), nullptr);
}

TEST(Project_PianomaniaCoordinatedExportTest, HeldNotePitchCurveV3ReconcilesOnlyAStaleFinalTickAfterTiesConnect)
{
    ScopedMScoreTestMode productionScoreSerialization(false);
    const QString fixturePath = QString::fromUtf8(project_test_DATA_ROOT)
                                + "/src/project/tests/data/pianomania-held-pitch-curve-v3-stale-end-470.mscx";
    QTemporaryDir utf8Directory;
    ASSERT_TRUE(utf8Directory.isValid());
    const QString utf8Subdirectory = utf8Directory.filePath(QString::fromUtf8("Inéz"));
    ASSERT_TRUE(QDir().mkpath(utf8Subdirectory));
    const QString utf8FixturePath = utf8Subdirectory + QString::fromUtf8("/Éyes Cut Deeper.mscx");
    ASSERT_TRUE(QFile::copy(fixturePath, utf8FixturePath));
    std::unique_ptr<mu::engraving::MasterScore> score(mu::engraving::ScoreRW::readScore(
        muse::String::fromUtf8(utf8FixturePath.toUtf8().constData()), true));
    ASSERT_NE(score, nullptr);

    const std::vector<mu::engraving::Note*> chain = notesWithPitch(score.get(), 45);
    ASSERT_EQ(chain.size(), 2u);
    const mu::engraving::PianomaniaHeldNotePitchCurve expected {
        { 0, 0, 0 }, { 961, -300, 0 }, { 2400, -300, 0 }
    };
    for (const mu::engraving::Note* note : chain) {
        EXPECT_EQ(note->pianomaniaHeldNoteDurationTicks(), 2400);
        EXPECT_EQ(note->pianomaniaHeldNotePitchCurve(), expected);
    }

    const auto exported = mu::project::pianomania::exportPianomaniaBundle(
        score.get(), muse::io::path_t(utf8FixturePath),
        muse::io::path_t(utf8Subdirectory + QString::fromUtf8("/résultat")),
        muse::io::path_t(utf8Subdirectory + QString::fromUtf8("/résultat.mei")), false);
    ASSERT_TRUE(exported.ret) << exported.ret.toString();
    EXPECT_EQ(QString::fromStdString(exported.val.source), QString::fromUtf8("Éyes Cut Deeper.mscx"));
    const QByteArray mei = readFile(utf8Subdirectory + QString::fromUtf8("/résultat.mei"));
    EXPECT_TRUE(mei.contains(QString::fromUtf8("Inéz").toUtf8()));
    EXPECT_EQ(mei.count("pm:heldPitchCurveV3=\"0:0:0;961:-300:0;2400:-300:0\""), 2);

    QTemporaryDir collisionDirectory;
    ASSERT_TRUE(collisionDirectory.isValid());
    const QByteArray source = readFile(fixturePath);
    ASSERT_FALSE(source.isEmpty());
    const QString collisionPath = writeModifiedFixture(source, "scoreTick=\"961\"", "scoreTick=\"2500\"",
                                                       &collisionDirectory);
    ASSERT_FALSE(collisionPath.isEmpty());
    EXPECT_EQ(mu::engraving::ScoreRW::readScore(
                  muse::String::fromUtf8(collisionPath.toUtf8().constData()), true), nullptr);
}

TEST(Project_PianomaniaCoordinatedExportTest, UnversionedHeldNotePitchCurveUsesTheV2MigrationPath)
{
    const QString fixturePath = QString::fromUtf8(project_test_DATA_ROOT)
                                + "/src/project/tests/data/pianomania-held-pitch-curve-v2-470.mscx";
    QByteArray legacy = readFile(fixturePath);
    ASSERT_FALSE(legacy.isEmpty());
    legacy.replace(" version=\"2\"", "");
    for (const auto& replacement : std::vector<std::pair<QByteArray, QByteArray> > {
             { "time=\"60000\"", "time=\"60\"" }, { "time=\"52000\"", "time=\"52\"" },
             { "time=\"45000\"", "time=\"45\"" }, { "time=\"42000\"", "time=\"42\"" },
             { "time=\"40000\"", "time=\"40\"" }, { "time=\"30000\"", "time=\"30\"" },
             { "time=\"8000\"", "time=\"8\"" }
         }) {
        legacy.replace(replacement.first, replacement.second);
    }
    legacy.replace(" slope=\"-3273\"", "");
    legacy.replace(" slope=\"-301\"", "");
    legacy.replace(" slope=\"128\"", "");
    legacy.replace(" slope=\"100\"", "");
    legacy.replace(" slope=\"0\"", "");

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = writeModifiedFixture(legacy, "unchanged-sentinel", "unchanged-sentinel", &directory);
    ASSERT_FALSE(path.isEmpty());
    std::unique_ptr<mu::engraving::MasterScore> score(
        mu::engraving::ScoreRW::readScore(muse::String::fromUtf8(path.toUtf8().constData()), true));
    ASSERT_NE(score, nullptr);
    ASSERT_EQ(notesWithPitch(score.get(), 60).size(), 3u);
    EXPECT_EQ(notesWithPitch(score.get(), 60).front()->pianomaniaHeldNotePitchCurve().back().scoreTick, 5760);

    const QString savedPath = directory.filePath("legacy-migrated.mscx");
    ASSERT_TRUE(mu::engraving::ScoreRW::saveScore(score.get(), muse::String::fromUtf8(savedPath.toUtf8().constData())));
    const QByteArray saved = readFile(savedPath);
    EXPECT_EQ(saved.count("pianomaniaHeldNotePitchCurve version=\"2\""), 0);
    EXPECT_EQ(saved.count("pianomaniaHeldNotePitchCurve version=\"3\""), 6);
}

TEST(Project_PianomaniaCoordinatedExportTest, HeldNotePitchCurveV2RejectsTickCollisionAndSlopeOverflow)
{
    using Migration = mu::engraving::compat::PianomaniaHeldNotePitchCurveMigration;
    mu::engraving::PianomaniaHeldNotePitchCurve migrated;
    muse::String error;

    const Migration::RawCurve collision { { 0, 0, 0 }, { 1, 100, 0 }, { 60000, 0, 0 } };
    EXPECT_FALSE(Migration::migrateCurve(collision, 30, &migrated, &error));
    EXPECT_NE(error.toStdString().find("duplicate score ticks"), std::string::npos);

    const Migration::RawCurve overflow { { 0, 0, 1000000 }, { 30000, 100, 1000000 }, { 60000, 0, 1000000 } };
    EXPECT_FALSE(Migration::migrateCurve(overflow, 30, &migrated, &error));
    EXPECT_NE(error.toStdString().find("slope overflow"), std::string::npos);

    EXPECT_FALSE(Migration::migrateCurve(overflow, 0, &migrated, &error));
    EXPECT_NE(error.toStdString().find("zero duration"), std::string::npos);
}

TEST(Project_PianomaniaCoordinatedExportTest, HeldNotePitchCurveReaderRejectsUnknownVersion)
{
    const QString fixturePath = QString::fromUtf8(project_test_DATA_ROOT)
                                + "/src/project/tests/data/pianomania-held-pitch-curve-v2-470.mscx";
    const QByteArray source = readFile(fixturePath);
    ASSERT_FALSE(source.isEmpty());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = writeModifiedFixture(source, "version=\"2\"", "version=\"99\"", &directory);
    ASSERT_FALSE(path.isEmpty());
    EXPECT_EQ(mu::engraving::ScoreRW::readScore(muse::String::fromUtf8(path.toUtf8().constData()), true), nullptr);
}

TEST(Project_PianomaniaCoordinatedExportTest, InvalidHeldNotePitchCurveStopsCoordinatedExport)
{
    ScopedMScoreTestMode productionScoreSerialization(false);
    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/importexport/midi/tests/midiexport_data/pianomania_trill_ordinary_notes.mscx";
    std::unique_ptr<mu::engraving::MasterScore> score(mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);
    mu::engraving::Note* heldNote = nullptr;
    score->scanElements([&heldNote](mu::engraving::EngravingItem* item) {
        if (!heldNote && item && item->isNote()) {
            heldNote = mu::engraving::toNote(item);
        }
    });
    ASSERT_NE(heldNote, nullptr);
    heldNote->setPianomaniaHeldNote(true);
    const int durationTicks = heldNote->pianomaniaHeldNoteDurationTicks();
    ASSERT_GT(durationTicks, 0);
    heldNote->setPianomaniaHeldNotePitchCurve({ { 0, 0, 0 }, { durationTicks + 1, -100, 0 } });

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto result = mu::project::pianomania::exportPianomaniaBundle(
        score.get(), muse::io::path_t(fixturePath), muse::io::path_t(directory.filePath("song")),
        muse::io::path_t(directory.filePath("song.mei")), false);
    EXPECT_FALSE(result.ret);
}

TEST(Project_PianomaniaCoordinatedExportTest, RejectsFourVisibleStaffTopology)
{
    ScopedMScoreTestMode productionScoreSerialization(false);

    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/importexport/midi/tests/midiimport_data/instrument_grand2.mscx";
    std::unique_ptr<mu::engraving::MasterScore> score(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const muse::RetVal<mu::project::pianomania::PianomaniaExportResult> result
        = mu::project::pianomania::exportPianomaniaBundle(
            score.get(), muse::io::path_t(fixturePath),
            muse::io::path_t(directory.filePath("song")),
            muse::io::path_t(directory.filePath("song.mei")), false);

    EXPECT_FALSE(result.ret);
    EXPECT_NE(result.ret.toString().find("exactly one visible score part"), std::string::npos)
        << result.ret.toString();
}

TEST(Project_PianomaniaCoordinatedExportTest, HiddenThirdStaffIsExcludedFromMeiAndMidi)
{
    ScopedMScoreTestMode productionScoreSerialization(false);

    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/importexport/midi/tests/midiimport_data/instrument_3staff_organ.mscx";
    std::unique_ptr<mu::engraving::MasterScore> score(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);
    ASSERT_EQ(score->staves().size(), 3);
    score->staves()[2]->setVisible(false);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const muse::RetVal<mu::project::pianomania::PianomaniaExportResult> result
        = mu::project::pianomania::exportPianomaniaBundle(
            score.get(), muse::io::path_t(fixturePath),
            muse::io::path_t(directory.filePath("song")),
            muse::io::path_t(directory.filePath("song.mei")), false);
    ASSERT_TRUE(result.ret) << result.ret.toString();
    ASSERT_EQ(result.val.canonicalEvents.size(), 2);
    ASSERT_EQ(result.val.files.size(), 1);
    EXPECT_EQ(result.val.files[0].audibleEvents.size(), 2);

    for (const auto& event : result.val.canonicalEvents) {
        EXPECT_TRUE(event.staff == 1 || event.staff == 2);
    }
    const QByteArray mei = readFile(directory.filePath("song.mei"));
    EXPECT_FALSE(mei.contains("<staff n=\"3\""));
    EXPECT_FALSE(mei.contains("<staffDef n=\"3\""));
}

TEST(Project_PianomaniaCoordinatedExportTest, HiddenPartIsExcludedFromMeiAndMidi)
{
    ScopedMScoreTestMode productionScoreSerialization(false);

    const muse::String fixturePath = muse::String::fromUtf8(project_test_DATA_ROOT)
                                     + u"/src/importexport/midi/tests/midiimport_data/instrument_grand2.mscx";
    std::unique_ptr<mu::engraving::MasterScore> score(
        mu::engraving::ScoreRW::readScore(fixturePath, true));
    ASSERT_NE(score, nullptr);
    ASSERT_EQ(score->parts().size(), 2);
    score->parts()[1]->setShow(false);

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const muse::RetVal<mu::project::pianomania::PianomaniaExportResult> result
        = mu::project::pianomania::exportPianomaniaBundle(
            score.get(), muse::io::path_t(fixturePath),
            muse::io::path_t(directory.filePath("song")),
            muse::io::path_t(directory.filePath("song.mei")), false);
    ASSERT_TRUE(result.ret) << result.ret.toString();
    ASSERT_EQ(result.val.canonicalEvents.size(), 4);
    ASSERT_EQ(result.val.files.size(), 1);
    EXPECT_EQ(result.val.files[0].audibleEvents.size(), 4);

    for (const auto& event : result.val.canonicalEvents) {
        EXPECT_TRUE(event.staff == 1 || event.staff == 2);
    }
    const QByteArray mei = readFile(directory.filePath("song.mei"));
    EXPECT_FALSE(mei.contains("<staff n=\"3\""));
    EXPECT_FALSE(mei.contains("<staff n=\"4\""));
    EXPECT_FALSE(mei.contains("<staffDef n=\"3\""));
    EXPECT_FALSE(mei.contains("<staffDef n=\"4\""));
}

TEST_P(Project_PianomaniaExportTest, ManifestV3FixtureMatrix)
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
    EXPECT_FALSE(QFileInfo::exists(firstDirectory.filePath("song-no-repeats.mei")));
    EXPECT_FALSE(QFileInfo::exists(firstDirectory.filePath("song-repeats.mei")));
    EXPECT_EQ(readFile(firstDirectory.filePath("manifest.json")), readFile(secondDirectory.filePath("manifest.json")));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(first.val.manifestJson), &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError) << parseError.errorString().toStdString();
    ASSERT_TRUE(document.isObject());
    const QJsonObject root = document.object();
    EXPECT_EQ(root.value("schema_version").toString(), MANIFEST_SCHEMA);
    const QByteArray exportedMei = readFile(firstDirectory.filePath("song.mei"));
    const std::set<QString> declaredPublicIdentities = verifyFinalPublicIdentityGraph(exportedMei, root);
    EXPECT_FALSE(declaredPublicIdentities.empty());
    if (std::string(expectation.name) == "played_chromatic_glissando") {
        EXPECT_TRUE(exportedMei.contains("<gliss"));
        EXPECT_TRUE(exportedMei.contains("startid=\"#pm-score-"));
        EXPECT_TRUE(exportedMei.contains("endid=\"#pm-score-"));
    }

    const QJsonObject scoreObject = root.value("score").toObject();
    EXPECT_EQ(scoreObject.value("base_path").toString(), "song");
    EXPECT_EQ(scoreObject.value("mei").toObject().value("path").toString(), "song.mei");
    EXPECT_TRUE(scoreObject.value("mei_no_repeats").isUndefined());
    EXPECT_TRUE(scoreObject.value("mei_repeats").isUndefined());

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
        EXPECT_TRUE(file.value("mei_path").isUndefined());
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

        if (std::string(expectation.name) == "gameplay_turn_anchor_return"
            || std::string(expectation.name) == "gameplay_tremblement_anchor_return") {
            verifyVisualAnchorsPrecedeOrnaments(file, ornamentDefinitions);
        }

        if (std::string(expectation.name) == "after_grace_cross_voice") {
            std::vector<std::pair<int, int> > pitchOrder;
            for (const QJsonValue& value : file.value("audible_events").toArray()) {
                const QJsonObject event = value.toObject();
                if (event.value("heard_midi_key").toInt() != 68) {
                    continue;
                }
                pitchOrder.push_back({
                    event.value("note_on_locator").toObject().value("absolute_tick").toInt(),
                    event.value("idx").toInt(-1),
                });
            }
            std::sort(pitchOrder.begin(), pitchOrder.end());
            ASSERT_EQ(pitchOrder.size(), 8u);
            for (size_t pitchIndex = 0; pitchIndex < pitchOrder.size(); ++pitchIndex) {
                EXPECT_EQ(pitchOrder[pitchIndex].second, static_cast<int>(pitchIndex));
            }
            EXPECT_EQ(countCanonicalEvents(canonical, "event_type", "written_grace"), 1);
            const auto grace = std::find_if(canonical.cbegin(), canonical.cend(), [](const QJsonValue& value) {
                return value.toObject().value("event_type").toString() == "written_grace";
            });
            ASSERT_NE(grace, canonical.cend());
            EXPECT_EQ(grace->toObject().value("idx").toInt(), 6);
            EXPECT_EQ(pitchOrder[6].second, grace->toObject().value("idx").toInt());
            EXPECT_EQ(pitchOrder[7].second, 7)
                << "The delayed same-pitch written anchor must follow its after-grace";
        }

        if (std::string(expectation.name) == "trill_after_grace_restrike") {
            struct WrittenPitchEvent {
                int tick = 0;
                int idx = -1;
                QString eventType;
            };
            std::vector<WrittenPitchEvent> writtenPitchOrder;
            for (const QJsonValue& value : file.value("audible_events").toArray()) {
                const QJsonObject event = value.toObject();
                if (event.value("heard_midi_key").toInt() != 60
                    || event.value("event_type").toString() == "nonvisual_ornament") {
                    continue;
                }
                writtenPitchOrder.push_back({
                    event.value("note_on_locator").toObject().value("absolute_tick").toInt(),
                    event.value("idx").toInt(-1),
                    event.value("event_type").toString(),
                });
            }
            std::sort(writtenPitchOrder.begin(), writtenPitchOrder.end(), [](const auto& left, const auto& right) {
                return left.tick < right.tick;
            });
            ASSERT_EQ(writtenPitchOrder.size(), 2u);
            EXPECT_EQ(writtenPitchOrder[0].eventType, "written");
            EXPECT_EQ(writtenPitchOrder[0].idx, 0);
            EXPECT_EQ(writtenPitchOrder[1].eventType, "written_grace");
            EXPECT_EQ(writtenPitchOrder[1].idx, 1);
        }

        if (std::string(expectation.name) == "short_trill_written_attack") {
            struct OrnamentAttack {
                int tick = 0;
                int pitch = -1;
                int idx = -1;
                int noteOffVelocity = -1;
                QString eventType;
                QString audibleEventId;
            };
            std::vector<OrnamentAttack> attacks;
            for (const QJsonValue& value : file.value("audible_events").toArray()) {
                const QJsonObject event = value.toObject();
                attacks.push_back({
                    event.value("note_on_locator").toObject().value("absolute_tick").toInt(),
                    event.value("heard_midi_key").toInt(),
                    event.value("idx").toInt(-1),
                    event.value("note_off_velocity").toInt(-1),
                    event.value("event_type").toString(),
                    event.value("audible_event_id").toString(),
                });
            }
            std::sort(attacks.begin(), attacks.end(), [](const auto& left, const auto& right) {
                return left.tick < right.tick;
            });
            ASSERT_EQ(attacks.size(), 3u);
            EXPECT_EQ(attacks[0].pitch, 68);
            EXPECT_EQ(attacks[0].idx, 0);
            EXPECT_EQ(attacks[0].eventType, "written");
            EXPECT_NE(attacks[0].noteOffVelocity, 127);
            ASSERT_EQ(file.value("visual_occurrences").toArray().size(), 1);
            const QJsonObject visualOccurrence = file.value("visual_occurrences").toArray().at(0).toObject();
            EXPECT_EQ(visualOccurrence.value("audible_event_id").toString(), attacks[0].audibleEventId);
            EXPECT_TRUE(canonicalIds.count(visualOccurrence.value("score_event_id").toString()));
            EXPECT_EQ(attacks[1].pitch, 69);
            EXPECT_EQ(attacks[1].idx, -1);
            EXPECT_EQ(attacks[1].eventType, "nonvisual_ornament");
            EXPECT_EQ(attacks[1].noteOffVelocity, 127);
            EXPECT_EQ(attacks[2].pitch, 68);
            EXPECT_EQ(attacks[2].idx, -1);
            EXPECT_EQ(attacks[2].eventType, "nonvisual_ornament");
            EXPECT_EQ(attacks[2].noteOffVelocity, 127);
        }

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
            { { "base", 20, 20, 0 }, { "repeats", 28, 28, 0 } }
        },
        ExportCase {
            "written_grace",
            "src/importexport/mei/tests/data/pianomania-grace-same-pitch-index.mscx",
            false, false, 0, 4, 2, 0, 0, 2,
            { { "base", 6, 4, 2 } }
        },
        ExportCase {
            "after_grace_cross_voice",
            "src/importexport/midi/tests/midiexport_data/pianomania_after_grace_cross_voice_order.mscx",
            false, false, 0, 8, 1, 0, 0, 0,
            { { "base", 8, 8, 0 } }
        },
        ExportCase {
            "ornament",
            "src/importexport/midi/tests/midiexport_data/pianomania_trill_basic_line.mscx",
            false, false, 0, 1, 0, 0, 0, 23,
            { { "base", 24, 1, 23 } }
        },
        ExportCase {
            "short_trill_written_attack",
            "src/importexport/midi/tests/midiexport_data/pianomania_short_trill_written_attack.mscx",
            false, false, 0, 1, 0, 0, 0, 2,
            { { "base", 3, 1, 2 } }
        },
        ExportCase {
            "played_chromatic_glissando",
            "src/importexport/midi/tests/midiexport_data/pianomania_played_chromatic_glissando.mscx",
            false, false, 0, 2, 0, 0, 0, 11,
            { { "base", 13, 2, 11 } }
        },
        ExportCase {
            "gameplay_turn_anchor_return",
            "src/importexport/midi/tests/midiexport_data/pianomania_gameplay_turn_anchor_return.mscx",
            false, false, 0, 2, 0, 0, 0, 8,
            { { "base", 10, 2, 8 } }
        },
        ExportCase {
            "gameplay_tremblement_anchor_return",
            "src/importexport/midi/tests/midiexport_data/pianomania_gameplay_tremblement_anchor_return.mscx",
            false, false, 0, 1, 0, 0, 0, 4,
            { { "base", 5, 1, 4 } }
        },
        ExportCase {
            "written_note_masks_coincident_ornament_attack",
            "src/importexport/midi/tests/midiexport_data/pianomania_trill_visual_collision.mscx",
            false, false, 0, 2, 0, 0, 0, 22,
            { { "base", 24, 2, 22 } }
        },
        ExportCase {
            "ties",
            "src/importexport/mei/tests/data/tie-01.mscx",
            false, false, 0, 22, 0, 0, 14, 0,
            { { "base", 8, 22, 0 } }
        },
        ExportCase {
            "cross_voice_tie",
            "src/importexport/midi/tests/midiexport_data/pianomania_cross_voice_tie.mscx",
            false, false, 0, 2, 0, 0, 1, 0,
            { { "base", 1, 2, 0 } }
        },
        ExportCase {
            "cross_staff_trill_restrike",
            "src/importexport/midi/tests/midiexport_data/testTrillCrossStaff.mscx",
            false, false, 0, 4, 0, 0, 0, 44,
            { { "base", 48, 4, 44 } }
        },
        ExportCase {
            "trill_after_grace_restrike",
            "src/importexport/midi/tests/midiexport_data/pianomania_trill_after_grace_canonical.mscx",
            false, false, 0, 3, 2, 0, 0, 22,
            { { "base", 25, 3, 22 } }
        },
        ExportCase {
            "unison_same_onset_alias",
            "src/importexport/midi/tests/midiexport_data/pianomania_same_start_unison.mscx",
            false, false, 0, 2, 0, 1, 0, 0,
            { { "base", 1, 2, 0 } }
        },
        ExportCase {
            "canonical_playback_authority",
            "src/project/tests/data/pianomania-playback-authority.mscx",
            false, false, 0, 2, 0, 0, 0, 0,
            { { "base", 2, 2, 0 } }
        }
        ),
    [](const ::testing::TestParamInfo<ExportCase>& info) {
        return info.param.name;
    }
    );
