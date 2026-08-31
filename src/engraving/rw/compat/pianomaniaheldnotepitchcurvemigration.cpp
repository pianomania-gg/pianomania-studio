/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */

#include "pianomaniaheldnotepitchcurvemigration.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "../../dom/note.h"
#include "../../types/pianomaniaheldnotepitchcurve.h"
#include "../../types/pitchvalue.h"
#include "../xmlreader.h"

using namespace mu::engraving;
using namespace mu::engraving::compat;

namespace {
constexpr int64_t V2_MAX_TIME = 60000;
constexpr int64_t SCORE_TICKS_PER_QUARTER = 480;

bool fail(muse::String* error, const muse::String& message)
{
    if (error) {
        *error = message;
    }
    return false;
}

bool failForNote(muse::String* error, const Note* note, const muse::String& reason)
{
    if (!note) {
        return fail(error, reason);
    }
    return fail(error, muse::String(u"Pianomania Held Note pitch curve at score tick %1, track %2, MIDI pitch %3: %4")
                .arg(note->tick().ticks()).arg(note->track()).arg(note->pitch()).arg(reason));
}

bool readIntegerAttribute(const XmlReader& xml, const char* name, int64_t* value)
{
    bool ok = false;
    const int parsed = xml.asciiAttribute(name).toInt(&ok);
    if (ok) {
        *value = parsed;
    }
    return ok;
}

int64_t roundRatio(int64_t numerator, int64_t denominator)
{
    const int64_t quotient = numerator / denominator;
    const int64_t remainder = numerator % denominator;
    if (std::abs(remainder) * 2 < denominator) {
        return quotient;
    }
    return quotient + (numerator < 0 ? -1 : 1);
}

bool validateRawCurve(const PianomaniaHeldNotePitchCurveMigration::RawCurve& curve, muse::String* error)
{
    if (curve.size() < 2 || curve.size() > 32 || curve.front().time != 0 || curve.front().pitch != 0
        || curve.back().time != V2_MAX_TIME) {
        return fail(error, u"Invalid Pianomania Held Note version 2 pitch curve endpoints or point count.");
    }

    int64_t previousTime = -1;
    bool hasNonzeroPitch = false;
    for (const auto& point : curve) {
        if (point.time < 0 || point.time > V2_MAX_TIME || point.time <= previousTime
            || point.pitch < -PianomaniaHeldNotePitchCurvePoint::MAX_PITCH_CENTS
            || point.pitch > PianomaniaHeldNotePitchCurvePoint::MAX_PITCH_CENTS
            || point.slope < -PianomaniaHeldNotePitchCurvePoint::MAX_SLOPE_CENTS_PER_QUARTER
            || point.slope > PianomaniaHeldNotePitchCurvePoint::MAX_SLOPE_CENTS_PER_QUARTER) {
            return fail(error, u"Invalid Pianomania Held Note version 2 pitch curve range.");
        }
        previousTime = point.time;
        hasNonzeroPitch = hasNonzeroPitch || point.pitch != 0;
    }
    if (!hasNonzeroPitch) {
        return fail(error, u"Invalid Pianomania Held Note version 2 pitch curve pitch range.");
    }
    return true;
}

bool deriveLegacyCurve(const std::vector<std::pair<int64_t, int64_t> >& legacy,
                       PianomaniaHeldNotePitchCurveMigration::RawCurve* curve, muse::String* error)
{
    if (legacy.size() < 2 || legacy.size() > 32 || legacy.front().first != 0 || legacy.front().second != 0
        || legacy.back().first != PitchValue::MAX_TIME) {
        return fail(error, u"Invalid unversioned Pianomania Held Note pitch curve.");
    }

    bool hasNonzeroPitch = false;
    int64_t previousTime = -1;
    for (const auto& point : legacy) {
        if (point.first < 0 || point.first > PitchValue::MAX_TIME || point.first <= previousTime
            || point.second < -PianomaniaHeldNotePitchCurvePoint::MAX_PITCH_CENTS
            || point.second > PianomaniaHeldNotePitchCurvePoint::MAX_PITCH_CENTS
            || point.second % 10 != 0) {
            return fail(error, u"Invalid unversioned Pianomania Held Note pitch curve range.");
        }
        previousTime = point.first;
        hasNonzeroPitch = hasNonzeroPitch || point.second != 0;
    }
    if (!hasNonzeroPitch) {
        return fail(error, u"Invalid unversioned Pianomania Held Note pitch curve pitch range.");
    }

    std::vector<double> derivatives(legacy.size(), 0.0);
    std::vector<double> secants;
    secants.reserve(legacy.size() - 1);
    for (size_t i = 0; i + 1 < legacy.size(); ++i) {
        const double h = static_cast<double>(legacy[i + 1].first - legacy[i].first) / PitchValue::MAX_TIME;
        secants.push_back((legacy[i + 1].second - legacy[i].second) / h);
    }
    if (legacy.size() == 2) {
        derivatives[0] = derivatives[1] = secants[0];
    } else {
        for (size_t i = 1; i + 1 < legacy.size(); ++i) {
            if (secants[i - 1] * secants[i] <= 0.0) {
                continue;
            }
            const double h0 = static_cast<double>(legacy[i].first - legacy[i - 1].first) / PitchValue::MAX_TIME;
            const double h1 = static_cast<double>(legacy[i + 1].first - legacy[i].first) / PitchValue::MAX_TIME;
            const double w0 = 2.0 * h1 + h0;
            const double w1 = h1 + 2.0 * h0;
            derivatives[i] = (w0 + w1) / (w0 / secants[i - 1] + w1 / secants[i]);
        }
        const auto endpoint = [](double h0, double h1, double d0, double d1) {
            const double candidate = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
            if (candidate * d0 <= 0.0) {
                return 0.0;
            }
            if (d0 * d1 < 0.0 && std::abs(candidate) > std::abs(3.0 * d0)) {
                return 3.0 * d0;
            }
            return candidate;
        };
        const double firstH = static_cast<double>(legacy[1].first - legacy[0].first) / PitchValue::MAX_TIME;
        const double secondH = static_cast<double>(legacy[2].first - legacy[1].first) / PitchValue::MAX_TIME;
        derivatives.front() = endpoint(firstH, secondH, secants[0], secants[1]);
        const size_t last = legacy.size() - 1;
        const double lastH = static_cast<double>(legacy[last].first - legacy[last - 1].first) / PitchValue::MAX_TIME;
        const double previousH = static_cast<double>(legacy[last - 1].first - legacy[last - 2].first) / PitchValue::MAX_TIME;
        derivatives.back() = endpoint(lastH, previousH, secants.back(), secants[secants.size() - 2]);
    }

    curve->clear();
    curve->reserve(legacy.size());
    for (size_t i = 0; i < legacy.size(); ++i) {
        const double boundedSlope = std::clamp(
            derivatives[i],
            -static_cast<double>(PianomaniaHeldNotePitchCurvePoint::MAX_SLOPE_CENTS_PER_QUARTER),
            static_cast<double>(PianomaniaHeldNotePitchCurvePoint::MAX_SLOPE_CENTS_PER_QUARTER));
        const int64_t slope = std::llround(boundedSlope);
        curve->push_back({ legacy[i].first * 1000, legacy[i].second, slope });
    }
    return validateRawCurve(*curve, error);
}
}

void PianomaniaHeldNotePitchCurveMigration::read(Note* note, XmlReader& xml)
{
    const bool legacy = !xml.hasAttribute("version");
    const muse::String version = xml.attribute("version");
    if (!legacy && version != u"2" && version != u"3") {
        xml.raiseError(muse::mtrc("engraving", "Unsupported Pianomania Held Note pitch curve version."));
        xml.skipCurrentElement();
        return;
    }

    if (version == u"3") {
        PianomaniaHeldNotePitchCurve curve;
        while (xml.readNextStartElement()) {
            if (xml.name() != "point" || !xml.hasAttribute("scoreTick") || !xml.hasAttribute("pitchCents")
                || !xml.hasAttribute("slopeCentsPerQuarter")) {
                xml.raiseError(muse::mtrc("engraving", "Invalid Pianomania Held Note pitch curve."));
                return;
            }
            curve.emplace_back(xml.intAttribute("scoreTick"), xml.intAttribute("pitchCents"),
                               xml.intAttribute("slopeCentsPerQuarter"));
            xml.readNext();
        }
        if (!Note::isValidPianomaniaHeldNotePitchCurve(curve) || curve.empty()) {
            xml.raiseError(muse::mtrc("engraving", "Invalid Pianomania Held Note pitch curve."));
        } else {
            // Ties are connected after the XML reader finishes. Stage version 3
            // curves so duration reconciliation and chain validation use the
            // complete authored tie chain.
            m_stagedVersion3Curves[note] = std::move(curve);
        }
        return;
    }

    RawCurve rawCurve;
    std::vector<std::pair<int64_t, int64_t> > legacyCurve;
    while (xml.readNextStartElement()) {
        if (xml.name() != "point" || !xml.hasAttribute("time") || !xml.hasAttribute("pitch")
            || (!legacy && !xml.hasAttribute("slope"))) {
            xml.raiseError(muse::mtrc("engraving", "Invalid Pianomania Held Note pitch curve."));
            return;
        }
        if (legacy) {
            int64_t time = 0;
            int64_t pitch = 0;
            if (!readIntegerAttribute(xml, "time", &time) || !readIntegerAttribute(xml, "pitch", &pitch)) {
                xml.raiseError(muse::mtrc("engraving", "Invalid Pianomania Held Note pitch curve integer."));
                return;
            }
            legacyCurve.emplace_back(time, pitch);
        } else {
            RawPoint point;
            if (!readIntegerAttribute(xml, "time", &point.time)
                || !readIntegerAttribute(xml, "pitch", &point.pitch)
                || !readIntegerAttribute(xml, "slope", &point.slope)) {
                xml.raiseError(muse::mtrc("engraving", "Invalid Pianomania Held Note pitch curve integer."));
                return;
            }
            rawCurve.push_back(point);
        }
        xml.readNext();
    }

    muse::String error;
    if ((legacy && !deriveLegacyCurve(legacyCurve, &rawCurve, &error))
        || (!legacy && !validateRawCurve(rawCurve, &error))) {
        xml.raiseError(error);
        return;
    }
    m_stagedVersion2Curves[note] = std::move(rawCurve);
}

bool PianomaniaHeldNotePitchCurveMigration::migrateCurve(const RawCurve& rawCurve, int durationTicks,
                                                         PianomaniaHeldNotePitchCurve* migratedCurve,
                                                         muse::String* error)
{
    if (durationTicks <= 0) {
        return fail(error, u"Cannot migrate a Pianomania Held Note pitch curve with zero duration.");
    }
    if (!validateRawCurve(rawCurve, error)) {
        return false;
    }

    PianomaniaHeldNotePitchCurve result;
    result.reserve(rawCurve.size());
    int64_t previousTick = -1;
    for (size_t i = 0; i < rawCurve.size(); ++i) {
        const auto& raw = rawCurve[i];
        int64_t scoreTick = roundRatio(raw.time * static_cast<int64_t>(durationTicks), V2_MAX_TIME);
        if (i == 0) {
            scoreTick = 0;
        } else if (i + 1 == rawCurve.size()) {
            scoreTick = durationTicks;
        }
        if (scoreTick <= previousTick) {
            return fail(error, u"Pianomania Held Note pitch curve migration produced duplicate score ticks.");
        }
        const int64_t slope = roundRatio(raw.slope * SCORE_TICKS_PER_QUARTER, durationTicks);
        if (scoreTick > std::numeric_limits<int>::max() || slope < std::numeric_limits<int>::min()
            || slope > std::numeric_limits<int>::max()
            || slope < -PianomaniaHeldNotePitchCurvePoint::MAX_SLOPE_CENTS_PER_QUARTER
            || slope > PianomaniaHeldNotePitchCurvePoint::MAX_SLOPE_CENTS_PER_QUARTER) {
            return fail(error, u"Pianomania Held Note pitch curve migration produced a slope overflow.");
        }
        result.emplace_back(static_cast<int>(scoreTick), static_cast<int>(raw.pitch), static_cast<int>(slope));
        previousTick = scoreTick;
    }
    if (!Note::isValidPianomaniaHeldNotePitchCurve(result, durationTicks)) {
        return fail(error, u"Pianomania Held Note pitch curve migration produced an invalid version 3 curve.");
    }
    *migratedCurve = std::move(result);
    return true;
}

bool PianomaniaHeldNotePitchCurveMigration::migrate(Score* score, muse::String* error)
{
    std::unordered_set<Note*> reconciledVersion3Notes;
    for (const auto& [stagedNote, stagedCurve] : m_stagedVersion3Curves) {
        if (stagedNote->score() != score || reconciledVersion3Notes.find(stagedNote) != reconciledVersion3Notes.end()) {
            continue;
        }
        const std::vector<Note*> chain = stagedNote->tiedNotes();
        if (chain.empty()) {
            return failForNote(error, stagedNote, u"the connected tie chain is empty.");
        }
        Note* chainStart = chain.front();
        for (Note* note : chain) {
            const auto found = m_stagedVersion3Curves.find(note);
            if (found == m_stagedVersion3Curves.end() || found->second != stagedCurve
                || !note->pianomaniaHeldNotePitchCurve().empty()) {
                return failForNote(error, chainStart,
                                   u"the connected tie chain has missing or inconsistent version 3 curves.");
            }
        }

        const int durationTicks = stagedNote->pianomaniaHeldNoteDurationTicks();
        PianomaniaHeldNotePitchCurve reconciledCurve = stagedCurve;
        const int authoredEndTick = reconciledCurve.back().scoreTick;
        if (authoredEndTick != durationTicks) {
            if (durationTicks <= 0 || std::any_of(reconciledCurve.cbegin(), reconciledCurve.cend() - 1,
                                                  [durationTicks](const PianomaniaHeldNotePitchCurvePoint& point) {
                return point.scoreTick >= durationTicks;
            })) {
                return failForNote(error, chainStart,
                                   muse::String(u"the stale final score tick %1 cannot reconcile to tie-chain duration %2 "
                                                "because an interior point reaches or exceeds the current duration.")
                                   .arg(authoredEndTick).arg(durationTicks));
            }
            reconciledCurve.back().scoreTick = durationTicks;
            if (!Note::isValidPianomaniaHeldNotePitchCurve(reconciledCurve, durationTicks)) {
                return failForNote(error, chainStart,
                                   muse::String(u"the stale final score tick %1 cannot reconcile to tie-chain duration %2 "
                                                "because the resulting curve is invalid.")
                                   .arg(authoredEndTick).arg(durationTicks));
            }
            LOGI() << "Reconciled Pianomania Held Note pitch curve final score tick at score tick"
                   << chainStart->tick().ticks() << ", track=" << chainStart->track()
                   << ", MIDI pitch=" << chainStart->pitch() << ", authored_end=" << authoredEndTick
                   << ", chain_duration=" << durationTicks;
        } else if (!Note::isValidPianomaniaHeldNotePitchCurve(reconciledCurve, durationTicks)) {
            return failForNote(error, chainStart, u"the version 3 curve is invalid for the connected tie-chain duration.");
        }

        for (Note* note : chain) {
            note->setPianomaniaHeldNotePitchCurve(reconciledCurve);
            reconciledVersion3Notes.insert(note);
        }
    }

    std::unordered_set<Note*> migratedNotes;
    for (const auto& [stagedNote, stagedCurve] : m_stagedVersion2Curves) {
        if (stagedNote->score() != score || migratedNotes.find(stagedNote) != migratedNotes.end()) {
            continue;
        }
        const std::vector<Note*> chain = stagedNote->tiedNotes();
        if (chain.empty()) {
            return fail(error, u"Pianomania Held Note pitch curve migration found an empty tie chain.");
        }
        for (Note* note : chain) {
            const auto found = m_stagedVersion2Curves.find(note);
            if (found == m_stagedVersion2Curves.end() || found->second != stagedCurve
                || !note->pianomaniaHeldNotePitchCurve().empty()) {
                return fail(error, u"Pianomania Held Note tie chain has missing or inconsistent version 2 pitch curves.");
            }
        }

        PianomaniaHeldNotePitchCurve migratedCurve;
        if (!migrateCurve(stagedCurve, stagedNote->pianomaniaHeldNoteDurationTicks(), &migratedCurve, error)) {
            return false;
        }
        for (Note* note : chain) {
            note->setPianomaniaHeldNotePitchCurve(migratedCurve);
            migratedNotes.insert(note);
        }
    }

    for (auto it = m_stagedVersion2Curves.begin(); it != m_stagedVersion2Curves.end();) {
        if (it->first->score() == score) {
            it = m_stagedVersion2Curves.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_stagedVersion3Curves.begin(); it != m_stagedVersion3Curves.end();) {
        if (it->first->score() == score) {
            it = m_stagedVersion3Curves.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}
