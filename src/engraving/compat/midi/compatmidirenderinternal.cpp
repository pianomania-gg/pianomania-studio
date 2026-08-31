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

/**
 \file
 render score into event list
*/

#include "compatmidirender.h"
#include "compatmidirenderinternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include "compat/midi/event.h"
#include "types/constants.h"

#include "dom/accidental.h"
#include "dom/arpeggio.h"
#include "dom/articulation.h"
#include "dom/bend.h"
#include "dom/chord.h"
#include "dom/durationtype.h"
#include "dom/dynamic.h"
#include "dom/fret.h"
#include "dom/glissando.h"
#include "dom/guitarbend.h"
#include "dom/hairpin.h"
#include "dom/harmony.h"
#include "dom/instrument.h"
#include "dom/interval.h"
#include "dom/letring.h"
#include "dom/masterscore.h"
#include "dom/measure.h"
#include "dom/measurerepeat.h"
#include "dom/note.h"
#include "dom/noteevent.h"
#include "dom/ornament.h"
#include "dom/part.h"
#include "dom/repeatlist.h"
#include "dom/score.h"
#include "dom/segment.h"
#include "dom/staff.h"
#include "dom/stafftextbase.h"
#include "dom/swing.h"
#include "dom/tie.h"
#include "dom/trill.h"
#include "dom/utils.h"
#include "dom/vibrato.h"
#include "dom/volta.h"

#include "log.h"

namespace mu::engraving {
static PitchWheelSpecs g_wheelSpec;
static constexpr int LET_RING_MAX_TICKS = Constants::DIVISION * 16;
static constexpr int DEFAULT_NOTE_OFF_VELOCITY = 64;
static constexpr int ORNAMENT_NOTE_OFF_VELOCITY = 127;
// TODO this should be a (configurable?) constant somewhere
static constexpr Fraction ARTICULATION_CHANGE_TIME_MAX = Fraction(1, 16);
std::unordered_map<String,
                   CompatMidiRendererInternal::Context::BuiltInArticulation> CompatMidiRendererInternal::Context::
s_builtInArticulationsValues = {
    { u"staccatissimo", { 1.0, 30 } },
    { u"staccato", { 1.0, 50 } },
    { u"portato", { 1.0, 67 } },
    { u"tenuto", { 1.0, 100 } },
    { u"accent", { 1.2, 100 } },
    { u"marcato", { 1.44, 100 } },
    { u"sforzato", { 1.69, 100 } },
};

struct CollectNoteParams {
    double velocityMultiplier = 1.;
    int tickOffset = 0;
    int graceOffsetOn = 0;
    int graceOffsetOff = 0;
    int endLetRingTick = 0;
    int previousChordTicks = -1;
    bool letRingNote = false;
    MidiInstrumentEffect effect = MidiInstrumentEffect::NONE;
    bool callAllSoundOff = false;//NoteOn silence channel
};

struct PlayNoteParams {
    int channel = 0;
    int pitch = 0;
    int velo = 0;
    int noteOffVelocity = DEFAULT_NOTE_OFF_VELOCITY;
    int onTime = 0;
    int offTime = 0;
    int offset = 0;
    int staffIdx = 0;
    MidiInstrumentEffect effect = MidiInstrumentEffect::NONE;
    bool callAllSoundOff = false;//NoteOn silence channel
};

struct VibratoParams {
    int lowPitch = 0;
    int highPitch = 0;
    int period = 0;
};

struct BendPlaybackInfo {
    int startTick = 0;
    int endTick = 0;
    float startTimeFactor = 0.f;
    float endTimeFactor = 1.f;
};

static double evaluatePianomaniaHeldPitchCurve(const PianomaniaHeldNotePitchCurve& curve, int relativeTick)
{
    if (relativeTick <= curve.front().scoreTick) {
        return curve.front().pitchCents;
    }
    if (relativeTick >= curve.back().scoreTick) {
        return curve.back().pitchCents;
    }

    auto upper = std::upper_bound(curve.begin(), curve.end(), relativeTick,
                                  [](int tick, const PianomaniaHeldNotePitchCurvePoint& point) {
        return tick < point.scoreTick;
    });
    const PianomaniaHeldNotePitchCurvePoint& b = *upper;
    const PianomaniaHeldNotePitchCurvePoint& a = *(upper - 1);
    const double segmentTicks = b.scoreTick - a.scoreTick;
    const double t = (relativeTick - a.scoreTick) / segmentTicks;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double durationQuarters = segmentTicks / Constants::DIVISION;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * a.pitchCents
           + (t3 - 2.0 * t2 + t) * durationQuarters * a.slopeCentsPerQuarter
           + (-2.0 * t3 + 3.0 * t2) * b.pitchCents
           + (t3 - t2) * durationQuarters * b.slopeCentsPerQuarter;
}

static double maximumAbsolutePianomaniaHeldPitchCents(const PianomaniaHeldNotePitchCurve& curve)
{
    double maximumAbsoluteCents = 0.0;
    constexpr double coefficientTolerance = 1e-12;

    for (size_t index = 1; index < curve.size(); ++index) {
        const PianomaniaHeldNotePitchCurvePoint& start = curve[index - 1];
        const PianomaniaHeldNotePitchCurvePoint& end = curve[index];
        const double durationQuarters = static_cast<double>(end.scoreTick - start.scoreTick) / Constants::DIVISION;
        const double startTangent = durationQuarters * start.slopeCentsPerQuarter;
        const double endTangent = durationQuarters * end.slopeCentsPerQuarter;

        // Hermite segment as a cubic polynomial a*t^3 + b*t^2 + c*t + d.
        const double a = 2.0 * start.pitchCents - 2.0 * end.pitchCents + startTangent + endTangent;
        const double b = -3.0 * start.pitchCents + 3.0 * end.pitchCents - 2.0 * startTangent - endTangent;
        const double c = startTangent;
        const double d = start.pitchCents;
        const auto includeValueAt = [&](double t) {
            const double value = ((a * t + b) * t + c) * t + d;
            maximumAbsoluteCents = std::max(maximumAbsoluteCents, std::abs(value));
        };

        includeValueAt(0.0);
        includeValueAt(1.0);

        // Interior extrema are the real roots of 3*a*t^2 + 2*b*t + c.
        const double derivativeA = 3.0 * a;
        const double derivativeB = 2.0 * b;
        if (std::abs(derivativeA) <= coefficientTolerance) {
            if (std::abs(derivativeB) > coefficientTolerance) {
                const double root = -c / derivativeB;
                if (root > 0.0 && root < 1.0) {
                    includeValueAt(root);
                }
            }
            continue;
        }

        const double discriminant = derivativeB * derivativeB - 4.0 * derivativeA * c;
        if (discriminant < 0.0) {
            continue;
        }
        const double squareRoot = std::sqrt(std::max(0.0, discriminant));
        const double roots[] = {
            (-derivativeB - squareRoot) / (2.0 * derivativeA),
            (-derivativeB + squareRoot) / (2.0 * derivativeA),
        };
        for (double root : roots) {
            if (root > 0.0 && root < 1.0) {
                includeValueAt(root);
            }
        }
    }

    return maximumAbsoluteCents;
}

static void collectPianomaniaHeldPitchCurve(const Note* note, int channel, int startTick,
                                            PitchWheelRenderer& pitchWheelRenderer, MidiInstrumentEffect effect)
{
    const PianomaniaHeldNotePitchCurve& curve = note->pianomaniaHeldNotePitchCurve();
    if (!note->pianomaniaHeldNote() || curve.empty() || note->tieBack()) {
        return;
    }

    const int durationTicks = note->pianomaniaHeldNoteDurationTicks();
    if (!Note::isValidPianomaniaHeldNotePitchCurve(curve, durationTicks)) {
        throw std::runtime_error("Invalid Pianomania held-note pitch curve reached MIDI rendering");
    }

    const double maximumAbsoluteCents = maximumAbsolutePianomaniaHeldPitchCents(curve);
    const int sensitivitySemitones = std::clamp(
        static_cast<int>(std::ceil((maximumAbsoluteCents - 1e-7) / 100.0)), 1, 24);
    const double sensitivityCents = sensitivitySemitones * 100.0;

    std::ostringstream identity;
    for (const PianomaniaHeldNotePitchCurvePoint& point : curve) {
        identity << point.scoreTick << ':' << point.pitchCents << ':' << point.slopeCentsPerQuarter << ';';
    }

    PitchWheelRenderer::PitchWheelFunction function;
    function.mStartTick = startTick;
    function.mEndTick = startTick + durationTicks;
    function.func = [curve, startTick, sensitivityCents](uint32_t absoluteTick) {
        const int relativeTick = static_cast<int>(absoluteTick) - startTick;
        const double cents = evaluatePianomaniaHeldPitchCurve(curve, relativeTick);
        const double normalized = cents / sensitivityCents;
        const double scale = normalized < 0.0 ? 8192.0 : 8191.0;
        return static_cast<int>(std::lround(normalized * scale));
    };
    pitchWheelRenderer.addPianomaniaPitchWheelFunction(function, channel, note->staffIdx(), effect,
                                                       identity.str(), sensitivitySemitones);
}

static uint32_t getChannel(const Instrument* instr, const Note* note, MidiInstrumentEffect effect,
                           const CompatMidiRendererInternal::Context& context);

static void fillScoreVelocities(const Score* score, CompatMidiRendererInternal::Context& context);
static void fillHairpinVelocities(const Hairpin* h, std::unordered_map<staff_idx_t, VelocityMap>& velocitiesByTrack);
static void fillVoltaVelocities(const Volta* volta, VelocityMap& veloMap);

static double chordVelocityMultiplier(const Chord* chord, const CompatMidiRendererInternal::Context& context);
static double velocityMultiplierByInstrument(const Instrument* instrument, const String& articulationName,
                                             const CompatMidiRendererInternal::Context& context);
static int graceBendDuration(const Chord* chord);
static Trill* findFirstTrill(Chord* chord);

static int trillUpperPitchOffset(const Note* note, const Ornament* ornament)
{
    if (!note) {
        return 0;
    }

    if (ornament && ornament->hasIntervalAbove()) {
        const OrnamentInterval interval = ornament->intervalAbove();
        if (interval.type != IntervalType::AUTO) {
            return Interval::fromOrnamentInterval(interval).chromatic;
        }
    }

    return chromaticPitchSteps(note, note, 1);
}

static void addTrillReturn(NoteEventList& events, const size_t index)
{
    NoteEvent& upper = events[index];
    const int upperDuration = upper.len();
    if (upperDuration < 2) {
        return;
    }

    const int upperDurationBeforeReturn = upperDuration / 2;
    NoteEvent returnedAnchor = upper;
    upper.setLen(upperDurationBeforeReturn);
    returnedAnchor.setPitch(0);
    returnedAnchor.setOntime(upper.ontime() + upperDurationBeforeReturn);
    returnedAnchor.setLen(upperDuration - upperDurationBeforeReturn);
    events.insert(events.begin() + index + 1, returnedAnchor);
}

static std::vector<size_t> playableEventIndexes(const NoteEventList& events)
{
    std::vector<size_t> result;
    result.reserve(events.size());
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].play()) {
            result.push_back(i);
        }
    }
    return result;
}

static void normalizeGameplayTrillEvents(NoteEventList& events, const Note* note, const Ornament* ornament)
{
    const int upperPitchOffset = trillUpperPitchOffset(note, ornament);
    if (upperPitchOffset == 0) {
        return;
    }

    const std::vector<size_t> playableEvents = playableEventIndexes(events);

    if (playableEvents.empty()) {
        events.push_back(NoteEvent(0, 0, NoteEvent::NOTE_LENGTH));
        events.push_back(NoteEvent(upperPitchOffset, NoteEvent::NOTE_LENGTH / 3, NoteEvent::NOTE_LENGTH / 3));
        events.push_back(NoteEvent(0, (NoteEvent::NOTE_LENGTH * 2) / 3, NoteEvent::NOTE_LENGTH / 3));
        return;
    }

    if (playableEvents.size() == 1) {
        const size_t anchorIndex = playableEvents.front();
        NoteEvent& anchor = events[anchorIndex];
        const int anchorDuration = anchor.len();
        if (anchorDuration < 3) {
            return;
        }

        const int upperStart = anchor.ontime() + anchorDuration / 3;
        const int returnStart = anchor.ontime() + (anchorDuration * 2) / 3;
        NoteEvent upper = anchor;
        NoteEvent returnedAnchor = anchor;
        anchor.setPitch(0);
        anchor.setLen(upperStart - anchor.ontime());
        upper.setPitch(upperPitchOffset);
        upper.setOntime(upperStart);
        upper.setLen(returnStart - upperStart);
        returnedAnchor.setPitch(0);
        returnedAnchor.setOntime(returnStart);
        returnedAnchor.setLen(anchorDuration - (returnStart - anchor.ontime()));
        events.insert(events.begin() + anchorIndex + 1, upper);
        events.insert(events.begin() + anchorIndex + 2, returnedAnchor);
        return;
    }

    for (size_t i = 0; i < playableEvents.size(); ++i) {
        events[playableEvents[i]].setPitch(i % 2 == 0 ? 0 : upperPitchOffset);
    }

    if (playableEvents.size() == 2) {
        addTrillReturn(events, playableEvents.back());
    } else if (playableEvents.size() % 2 == 0) {
        events[playableEvents.back()].setPitch(0);
    }
}

static void prependGameplayAnchorToLeadingAuxiliary(NoteEventList& events)
{
    const std::vector<size_t> playableEvents = playableEventIndexes(events);
    if (playableEvents.empty()) {
        return;
    }

    const size_t leadingIndex = playableEvents.front();
    NoteEvent& leadingAuxiliary = events[leadingIndex];
    if (leadingAuxiliary.pitch() == 0) {
        return;
    }

    const int leadingDuration = leadingAuxiliary.len();
    if (leadingDuration < 2) {
        leadingAuxiliary.setPitch(0);
        return;
    }

    const int anchorDuration = leadingDuration / 2;
    NoteEvent retainedAuxiliary = leadingAuxiliary;
    leadingAuxiliary.setPitch(0);
    leadingAuxiliary.setLen(anchorDuration);
    retainedAuxiliary.setOntime(leadingAuxiliary.ontime() + anchorDuration);
    retainedAuxiliary.setLen(leadingDuration - anchorDuration);
    events.insert(events.begin() + leadingIndex + 1, retainedAuxiliary);
}

static bool isGameplayOrnament(SymId symId)
{
    switch (symId) {
    case SymId::ornamentTrill:
    case SymId::ornamentShortTrill:
    case SymId::ornamentTurn:
    case SymId::ornamentTurnInverted:
    case SymId::ornamentTurnSlash:
    case SymId::ornamentTremblement:
        return true;
    default:
        return false;
    }
}

static void normalizeGameplayOrnamentEvents(NoteEventList& events, const Note* note, const Ornament* ornament,
                                             SymId symId)
{
    switch (symId) {
    case SymId::ornamentTrill:
    case SymId::ornamentShortTrill:
        normalizeGameplayTrillEvents(events, note, ornament);
        return;
    case SymId::ornamentTurn:
    case SymId::ornamentTurnInverted:
    case SymId::ornamentTurnSlash:
    case SymId::ornamentTremblement:
        prependGameplayAnchorToLeadingAuxiliary(events);
        return;
    default:
        return;
    }
}

//---------------------------------------------------------
//   Converts midi time (noteoff - noteon) to milliseconds
//---------------------------------------------------------
int toMilliseconds(float tempo, float midiTime)
{
    float ticksPerSecond = (float)Constants::DIVISION * tempo;
    int time = (int)((midiTime / ticksPerSecond) * 1000.0f);
    if (time > 0x7fff) { //maximum possible value
        time = 0x7fff;
    }
    return time;
}

//---------------------------------------------------------
//   Detects if a note is a start of a glissando
//---------------------------------------------------------
bool isGlissandoFor(const Note* note)
{
    for (Spanner* spanner : note->spannerFor()) {
        if (spanner->isGlissando()) {
            return true;
        }
    }
    return false;
}

//---------------------------------------------------------
//   Detects if a note is an end of a glissando
//---------------------------------------------------------
bool isGlissandoBack(const Note* note)
{
    for (Spanner* spanner : note->spannerBack()) {
        if (spanner->isGlissando()) {
            return true;
        }
    }
    return false;
}

static bool hasGeneratedGlissandoPlayback(const Note* note)
{
    for (Spanner* spanner : note->spannerFor()) {
        if (!spanner->isGlissando()) {
            continue;
        }

        std::vector<int> pitchOffsets;
        if (Glissando::pitchSteps(spanner, pitchOffsets) && pitchOffsets.size() > 1) {
            return true;
        }
    }

    return false;
}

static bool isPlainStaccato(const Articulation* articulation)
{
    if (!articulation || !articulation->playArticulation()) {
        return false;
    }

    return articulation->symId() == SymId::articStaccatoAbove
           || articulation->symId() == SymId::articStaccatoBelow;
}

static bool hasOnlyPlainStaccato(const Chord* chord)
{
    bool hasStaccato = false;
    for (Articulation* articulation : chord->articulations()) {
        if (!isPlainStaccato(articulation)) {
            return false;
        }
        hasStaccato = true;
    }
    return hasStaccato;
}

static int staccatoMinimumDurationTicks(int writtenTicks)
{
    return std::max(1, writtenTicks / 4);
}

static int staccatoMaximumDurationTicks(int writtenTicks)
{
    return std::max(staccatoMinimumDurationTicks(writtenTicks), (writtenTicks * 35) / 100);
}

static int repairedStaccatoDurationTicks(int writtenTicks, int currentDurationTicks)
{
    return std::clamp(currentDurationTicks, staccatoMinimumDurationTicks(writtenTicks), staccatoMaximumDurationTicks(writtenTicks));
}

static void collectGlissando(int channel, MidiInstrumentEffect effect,
                             int onTime, int offTime,
                             int pitchDelta,
                             PitchWheelRenderer& pitchWheelRenderer, staff_idx_t staffIdx)
{
    const float scale = (float)g_wheelSpec.mLimit / g_wheelSpec.mAmplitude;

    PitchWheelRenderer::PitchWheelFunction func;
    func.mStartTick = onTime;
    func.mEndTick = offTime;

    auto linearFunc = [startTick = onTime, endTick = offTime, pitchDelta, scale] (uint32_t tick) {
        float x = (float)(tick - startTick) / (endTick - startTick);
        return pitchDelta * x * scale;
    };
    func.func = linearFunc;

    pitchWheelRenderer.addPitchWheelFunction(func, channel, staffIdx, effect);
}

static Fraction getPlayTicksForBend(const Note* note)
{
    Tie* tie = note->tieFor();
    if (!tie || !tie->endNote()) {
        return note->chord()->actualTicks();
    }

    Fraction stick = note->chord()->tick();
    Note* nextNote = tie->endNote();
    while (tie && tie->endNote()) {
        nextNote = tie->endNote();
        for (EngravingItem* e : nextNote->el()) {
            if (e && (e->isBend())) {
                return nextNote->chord()->tick() - stick;
            }
        }

        tie = nextNote->tieFor();
    }

    return nextNote->chord()->endTick() - stick;
}

//---------------------------------------------------------
//   playNote
//---------------------------------------------------------
static void playNote(EventsHolder& events, const Note* note, PlayNoteParams params, PitchWheelRenderer& pitchWheelRenderer)
{
    if (!note->play()) {
        return;
    }

    if (note->userVelocity() != 0) {
        params.velo = note->customizeVelocity(params.velo);
    }

    if (params.callAllSoundOff && params.onTime != 0) {
        NPlayEvent ev1(ME_CONTROLLER, params.channel, CTRL_ALL_NOTES_OFF, 0);
        ev1.setEffect(params.effect);
        events[params.channel].emplace(params.onTime - 1, ev1);
    }

    NPlayEvent ev(ME_NOTEON, params.channel, params.pitch, params.velo);
    ev.setOriginatingStaff(params.staffIdx);
    ev.setTuning(note->tuning());
    ev.setNote(note);
    ev.setEffect(params.effect);
    if (params.offTime > 0 && params.offTime < params.onTime) {
        return;
    }

    events[params.channel].emplace(std::max(0, params.onTime - params.offset), ev);
    Accidental* acc = note->accidental();
    if (acc) {
        AccidentalType type = acc->accidentalType();
        double cents = Accidental::subtype2centOffset(type);
        if (!muse::RealIsNull(cents)) {
            double pwValue = cents / 100.0 * (double)g_wheelSpec.mLimit / (double)g_wheelSpec.mAmplitude;
            PitchWheelRenderer::PitchWheelFunction func;
            func.mStartTick = params.onTime - params.offset;
            func.mEndTick = params.offTime - params.offset;
            auto microtonalPW = [pwValue](uint32_t tick) {
                UNUSED(tick);
                return static_cast<int>(std::round(pwValue));
            };
            func.func = microtonalPW;
            pitchWheelRenderer.addPitchWheelFunction(func, params.channel, params.staffIdx, MidiInstrumentEffect::NONE);
        }
    }
    // adds portamento for continuous glissando
    for (Spanner* spanner : note->spannerFor()) {
        if (spanner->isGlissando()) {
            Glissando* glissando = toGlissando(spanner);
            if (glissando->glissandoStyle() == GlissandoStyle::PORTAMENTO) {
                Note* nextNote = toNote(spanner->endElement());
                double pitchDelta = nextNote->ppitch() - params.pitch;
                int timeDelta = params.offTime - params.onTime;
                if (pitchDelta != 0 && timeDelta != 0) {
                    collectGlissando(params.channel, params.effect, params.onTime, params.offTime, pitchDelta, pitchWheelRenderer,
                                     glissando->staffIdx());
                }
            }
        }
    }

    if (params.offTime != -1) {
        NPlayEvent offEv(ME_NOTEOFF, params.channel, params.pitch, params.noteOffVelocity);
        offEv.setOriginatingStaff(params.staffIdx);
        offEv.setTuning(note->tuning());
        offEv.setNote(note);
        offEv.setEffect(params.effect);
        events[params.channel].emplace(std::max(0, params.offTime - params.offset), offEv);
    }
}

static void collectVibrato(int channel,
                           int onTime, int offTime,
                           const VibratoParams& vibratoParams,
                           PitchWheelRenderer& pitchWheelRenderer, MidiInstrumentEffect effect, staff_idx_t staffIdx)
{
    const uint16_t vibratoPeriod = vibratoParams.period;
    const uint32_t duration = offTime - onTime;
    const float scale = 2 * (float)g_wheelSpec.mLimit / g_wheelSpec.mAmplitude / 100;

    if (duration < vibratoPeriod) {
        return;
    }

    const int pillarAmplitude = (vibratoParams.highPitch - vibratoParams.lowPitch);

    PitchWheelRenderer::PitchWheelFunction func;
    func.mStartTick = onTime;
    func.mEndTick = offTime - duration % vibratoPeriod;//removed last points to make more smooth of the end

    int lowPitch = vibratoParams.lowPitch;
    auto vibratoFunc = [startTick = onTime, pillarAmplitude, vibratoPeriod, lowPitch, scale] (uint32_t tick) {
        float x = (float)(tick - startTick) / vibratoPeriod;
        return (pillarAmplitude * 2 / M_PI * asin(sin(2 * M_PI * x)) + lowPitch) * scale;
    };
    func.func = vibratoFunc;

    pitchWheelRenderer.addPitchWheelFunction(func, channel, staffIdx, effect);
}

static void addConstPitchWheel(int startTick, int endTick, float value, PitchWheelRenderer& pitchWheelRenderer, int channel,
                               staff_idx_t staffIdx,
                               MidiInstrumentEffect effect)
{
    const float scale = (float)g_wheelSpec.mLimit / g_wheelSpec.mAmplitude;

    PitchWheelRenderer::PitchWheelFunction pitchWheelConstFunc;
    auto constFunc = [value, scale] (uint32_t tick) {
        UNUSED(tick)
        return value * scale;
    };

    pitchWheelConstFunc.func = constFunc;
    pitchWheelConstFunc.mStartTick = startTick;
    pitchWheelConstFunc.mEndTick = endTick;
    pitchWheelRenderer.addPitchWheelFunction(pitchWheelConstFunc, channel, staffIdx, effect);
}

static bool shouldProceedBend(const Note* note)
{
    const GuitarBend* bendFor = note->bendFor();
    const Note* baseNote = bendFor->startNoteOfChain();

    const GuitarBend* firstBend = baseNote->bendFor();
    if (firstBend && firstBend->bendType() == GuitarBendType::PRE_BEND) {
        const Note* nextNote = firstBend->endNote();
        if (nextNote) {
            baseNote = nextNote;
        }
    }

    return baseNote->lastTiedNote(false) == note;
}

static BendPlaybackInfo getBendPlaybackInfo(const GuitarBend* bend, int bendStart, int bendDuration, bool graceBeforeBend)
{
    BendPlaybackInfo bendInfo;

    // currently ignoring diagram for "grace before" bends
    if (!graceBeforeBend) {
        bendInfo.startTimeFactor = bend->startTimeFactor();
        bendInfo.endTimeFactor = bend->endTimeFactor();
    }

    bendInfo.startTick = bendStart + bendDuration * bendInfo.startTimeFactor;
    bendInfo.endTick = bendStart + bendDuration * bendInfo.endTimeFactor;

    return bendInfo;
}

static void fillBendDurations(const Note* bendStartNote, const std::unordered_set<const Note*>& currentNotes,
                              std::unordered_map<const Note*, int>& durations)
{
    if (!bendStartNote || currentNotes.empty()) {
        return;
    }

    int eachBendDuration = bendStartNote->chord()->actualTicks().ticks() / static_cast<int>(currentNotes.size());

    for (const Note* note : currentNotes) {
        durations.insert({ note, eachBendDuration });
    }
}

static std::unordered_map<const Note*, int> getGraceNoteBendDurations(const Note* note)
{
    std::unordered_map<const Note*, int> durations;
    const Note* bendStartNote = nullptr;
    std::unordered_set<const Note*> currentNotes;

    if (note->bendFor() && note->bendFor()->bendType() == GuitarBendType::SLIGHT_BEND) {
        return {};
    }

    while (note->tieFor()) {
        const Tie* tieFor = note->tieFor();
        IF_ASSERT_FAILED(tieFor->endNote()) {
            LOGE() << "cannot find tied note for note on track " << note->track() << ", tick " << note->tick().ticks();
            return {};
        }
        note = tieFor->endNote();
    }

    while (note->bendFor()) {
        const GuitarBend* bendFor = note->bendFor();
        const Note* endNote = bendFor->endNote();
        if (!endNote || note == endNote) {
            LOGE() << "cannot find end bend note for note on track " << note->track() << ", tick " << note->tick().ticks();
            return {};
        }

        if (endNote->chord()->isGraceAfter()) {
            if (currentNotes.empty()) {
                IF_ASSERT_FAILED(note->chord() == endNote->chord()->explicitParent()) {
                    LOGE() << "error in filling bends midi data for note on track " << note->track() << ", tick " << note->tick().ticks();
                    return {};
                }
                bendStartNote = note;
                currentNotes.insert(bendStartNote);
            }

            if (endNote->bendFor()) {
                currentNotes.insert(endNote);
            }
        } else {
            fillBendDurations(bendStartNote, currentNotes, durations);
            bendStartNote = nullptr;
            currentNotes.clear();
        }

        note = bendFor->endNote();
    }

    fillBendDurations(bendStartNote, currentNotes, durations);

    return durations;
}

/*
 * All consecutive tie and bend combinations are processed in a single pass, adding pitch bends where needed to ensure continuity between notes.
 *
 * When processing the first bend in a series, the duration of any preceding ties is also included,
 * allowing for an accurate total duration calculation.
 *
 * Additional calls (for notes that have already been processed) are filtered out by the function shouldProceedBend(Note*),
 * preventing redundant processing.
*/
static void collectGuitarBend(const Note* note,
                              int channel,
                              int onTime, int graceOffset, int previousChordTicks,
                              PitchWheelRenderer& pitchWheelRenderer, MidiInstrumentEffect effect)
{
    if (!shouldProceedBend(note)) {
        return;
    }

    const auto& graceNoteBendDurations = getGraceNoteBendDurations(note);

    int curPitchBendSegmentStart = onTime;

    int quarterOffsetFromStartNote = 0;
    int currentQuarterTones = 0;

    if (note->bendFor()->bendType() == GuitarBendType::GRACE_NOTE_BEND) {
        curPitchBendSegmentStart -= graceOffset;
    }

    const float scale = (float)g_wheelSpec.mLimit / g_wheelSpec.mAmplitude;

    while (note->bendFor() || note->tieFor()) {
        GuitarBend* bendFor = note->bendFor();
        int duration = note->chord()->actualTicks().ticks();
        if (bendFor) {
            const Note* endNote = bendFor->endNote();
            if (!endNote) {
                return;
            }

            bool graceBeforeBend = false;
            if (note->chord()->isGraceBefore() && bendFor) {
                if (endNote->noteType() == NoteType::NORMAL) {
                    duration = (previousChordTicks == -1) ? GRACE_BEND_DURATION : std::min(previousChordTicks / 2, GRACE_BEND_DURATION);
                    graceBeforeBend = true;
                }
            } else if (muse::contains(graceNoteBendDurations, note)) {
                duration = graceNoteBendDurations.at(note);
            }

            BendPlaybackInfo bendPlaybackInfo = getBendPlaybackInfo(bendFor, curPitchBendSegmentStart, duration, graceBeforeBend);
            double initialPitchBendValue = quarterOffsetFromStartNote / 2.0;

            if (bendPlaybackInfo.startTick > curPitchBendSegmentStart && initialPitchBendValue != 0) {
                addConstPitchWheel(curPitchBendSegmentStart, bendPlaybackInfo.startTick, initialPitchBendValue, pitchWheelRenderer, channel,
                                   note->staffIdx(), effect);
            }

            bendFor->computeBendAmount();
            currentQuarterTones = bendFor->bendAmountInQuarterTones();

            double tickDelta = duration * (bendPlaybackInfo.endTimeFactor - bendPlaybackInfo.startTimeFactor);
            double a = currentQuarterTones / 2.0 / (tickDelta * tickDelta);
            double b = initialPitchBendValue;
            auto bendFunc = [startTick = bendPlaybackInfo.startTick, scale, a, b] (uint32_t tick) {
                float x = (float)(tick - startTick);
                float y = a * x * x + b;
                return y * scale;
            };

            PitchWheelRenderer::PitchWheelFunction pitchWheelSquareFunc;

            pitchWheelSquareFunc.func = bendFunc;

            pitchWheelSquareFunc.mStartTick = bendPlaybackInfo.startTick;
            pitchWheelSquareFunc.mEndTick = bendPlaybackInfo.endTick;

            pitchWheelRenderer.addPitchWheelFunction(pitchWheelSquareFunc, channel, note->staffIdx(), effect);
            quarterOffsetFromStartNote += currentQuarterTones;

            const int curPitchBendSegmentEnd = curPitchBendSegmentStart + duration;
            if (bendPlaybackInfo.endTick < curPitchBendSegmentEnd) {
                int constPitchWheelduration
                    = (quarterOffsetFromStartNote == 0 ? g_wheelSpec.mStep : curPitchBendSegmentEnd - bendPlaybackInfo.endTick);
                addConstPitchWheel(bendPlaybackInfo.endTick, bendPlaybackInfo.endTick + constPitchWheelduration,
                                   quarterOffsetFromStartNote / 2.0, pitchWheelRenderer, channel,
                                   note->staffIdx(),
                                   effect);
            }

            if (note == endNote) {
                break;
            }

            note = endNote;
        } else {
            if (!note->isGrace() && note->bendBack()) {
                int constPitchWheelduration = 0;
                int noteTick = note->tick().ticks();
                if (quarterOffsetFromStartNote == 0) {
                    // reset pitchwheel once, no need to keep in for each tick
                    constPitchWheelduration = g_wheelSpec.mStep;
                } else {
                    Note* lastTied = note->lastTiedNote(false);
                    IF_ASSERT_FAILED(lastTied) {
                        LOGE() << "couldn't find tied note for note on track " << note->track() << ", tick " << note->tick().ticks() <<
                            ", guitar bend midi may be incorrect";
                        constPitchWheelduration = note->chord()->actualTicks().ticks();
                    } else {
                        Chord* lastChord = lastTied->chord();
                        // keep the last pitchwheel value for the total duration of tied notes
                        constPitchWheelduration = lastChord->tick().ticks() - noteTick
                                                  + (lastTied->bendFor() ? 0 : lastChord->actualTicks().ticks());
                    }
                }

                addConstPitchWheel(noteTick, noteTick + constPitchWheelduration, quarterOffsetFromStartNote / 2.0, pitchWheelRenderer,
                                   channel,
                                   note->staffIdx(), effect);
            }

            const Tie* tie = note->tieFor();
            note = tie->endNote();
            if (!note) {
                break;
            }
        }

        curPitchBendSegmentStart += duration;
    }

    // adding pitch wheel to last note of bend/tie chain, if it's end of bend
    if (!note->isGrace() && note->bendBack()) {
        int constPitchWheelduration = (quarterOffsetFromStartNote == 0) ? g_wheelSpec.mStep : note->chord()->actualTicks().ticks();
        addConstPitchWheel(note->tick().ticks(),
                           note->tick().ticks() + constPitchWheelduration, quarterOffsetFromStartNote / 2.0, pitchWheelRenderer, channel,
                           note->staffIdx(), effect);
    }
}

static void collectBend(const PitchValues& playData, staff_idx_t staffIdx,
                        int channel,
                        int onTime, int offTime,
                        PitchWheelRenderer& pitchWheelRenderer, MidiInstrumentEffect effect)
{
    size_t pitchSize = playData.size();

    const float scale = 2 * (float)g_wheelSpec.mLimit / g_wheelSpec.mAmplitude / PitchValue::PITCH_FOR_SEMITONE;
    uint32_t duration = offTime - onTime;

    for (size_t i = 0; i < pitchSize - 1; i++) {
        PitchValue curValue = playData.at(i);
        PitchValue nextValue = playData.at(i + 1);

        //! y = a x^2 + b - curve
        float curTick = (float)curValue.time * duration / PitchValue::MAX_TIME;
        float nextTick = (float)nextValue.time * duration / PitchValue::MAX_TIME;

        float a = (float)(nextValue.pitch - curValue.pitch) / ((curTick - nextTick) * (curTick - nextTick));
        float b = curValue.pitch;

        uint32_t x0 = curValue.time * duration / PitchValue::MAX_TIME;

        PitchWheelRenderer::PitchWheelFunction func;
        func.mStartTick = onTime + x0;
        uint32_t startTimeNextPoint = nextValue.time * duration / PitchValue::MAX_TIME;
        func.mEndTick = onTime + startTimeNextPoint;

        auto bendFunc = [ startTick = func.mStartTick, scale,
                          a, b] (uint32_t tick) {
            float x = (float)(tick - startTick);

            float y = a * x * x + b;

            return y * scale;
        };
        func.func = bendFunc;
        pitchWheelRenderer.addPitchWheelFunction(func, channel, staffIdx, effect);
    }
    PitchWheelRenderer::PitchWheelFunction func;
    func.mStartTick = onTime + playData.at(pitchSize - 1).time * duration / PitchValue::MAX_TIME;
    func.mEndTick = offTime;

    if (func.mEndTick == func.mStartTick) {
        return;
    }

    //! y = releaseValue linear curve
    uint32_t releaseValue = playData.at(pitchSize - 1).pitch * scale;
    auto bendFunc = [releaseValue] (uint32_t tick) {
        UNUSED(tick)
        return releaseValue;
    };
    func.func = bendFunc;
    pitchWheelRenderer.addPitchWheelFunction(func, channel, staffIdx, effect);
}

static bool letRingShouldApply(const NoteEvent& event, const Note* note)
{
    if (note->hasSlideFromNote()) {
        return false;
    }

    if (isGlissandoBack(note)) {
        return true;
    }

    if (event.slide() || isGlissandoFor(note)) {
        return false;
    }

    return true;
}

static void renderSnd(EventsHolder& events, const Chord* chord, int noteChannel, int tickOffset,
                      const CompatMidiRendererInternal::Context& context)
{
    Fraction stick = chord->tick();
    Fraction etick = stick + chord->ticks();
    const VelocityMap& veloEvents = context.velocitiesByTrack.at(chord->track());
    const VelocityMap& multEvents = context.velocityMultiplicationsByTrack.at(chord->track());
    auto changes = veloEvents.changesInRange(stick, etick);
    auto multChanges = multEvents.changesInRange(stick, etick);

    std::map<int, int> velocityMap;
    for (auto& change : changes) {
        int lastVal = -1;
        int endPoint = change.second.ticks();
        for (int t = change.first.ticks(); t <= endPoint; t++) {
            int velo = veloEvents.val(Fraction::fromTicks(t));
            if (velo == lastVal) {
                continue;
            }
            lastVal = velo;

            velocityMap[t] = velo;
        }
    }

    double CONVERSION_FACTOR = CompatMidiRendererInternal::ARTICULATION_CONV_FACTOR;
    for (auto& change : multChanges) {
        // Ignore fix events: they are available as cached ramp starts
        // and considering them ends up with multiplying twice effectively
        if (change.first == change.second) {
            continue;
        }

        int lastVal = CompatMidiRendererInternal::ARTICULATION_CONV_FACTOR;
        int endPoint = change.second.ticks();
        int lastVelocity = 0;
        auto lastValocityIt = velocityMap.upper_bound(change.first.ticks());
        if (lastValocityIt != velocityMap.end()) {
            lastVelocity = lastValocityIt->second;
        } else if (!velocityMap.empty()) {
            lastVelocity = velocityMap.cbegin()->second;
        }

        for (int t = change.first.ticks(); t <= endPoint; t++) {
            int mult = multEvents.val(Fraction::fromTicks(t));
            if (mult == lastVal || mult == CONVERSION_FACTOR) {
                continue;
            }
            lastVal = mult;

            double realMult = mult / CONVERSION_FACTOR;
            if (velocityMap.find(t) != velocityMap.end()) {
                lastVelocity = velocityMap[t];
                velocityMap[t] *= realMult;
            } else {
                velocityMap[t] = lastVelocity * realMult;
            }
        }
    }

    for (auto point = velocityMap.cbegin(); point != velocityMap.cend(); ++point) {
        // NOTE:JT if we ever want to use poly aftertouch instead of CC, this is where we want to
        // be using it. Instead of ME_CONTROLLER, use ME_POLYAFTER (but duplicate for each note in chord)
        NPlayEvent event = NPlayEvent(ME_CONTROLLER, noteChannel, context.sndController, std::clamp(point->second, 0, 127));
        event.setOriginatingStaff(chord->staffIdx());
        events[noteChannel].insert(std::make_pair(point->first + tickOffset, event));
    }
}

int graceBendDuration(const Chord* chord)
{
    int graceDuration = GRACE_BEND_DURATION;
    if (chord) {
        graceDuration = std::min(chord->ticks().ticks() / 2, graceDuration);
    }

    return graceDuration;
}

static int calculateTieLength(const Note* note)
{
    int tieLen = 0;

    const Note* n = note;
    while (n) {
        // Process ties or bends
        const Tie* tieFor = n->tieForNonPartial();
        const GuitarBend* bendFor = n->bendFor();

        if (tieFor && tieFor->endNote() != n) {
            n = tieFor->endNote();
        } else if (bendFor && bendFor->endNote() != n) {
            n = bendFor->endNote();
        } else {
            break;
        }

        IF_ASSERT_FAILED(n) {
            break;
        }

        const NoteEventList& nel = n->playEvents();

        if (!nel.empty() && (!n->chord()->isGrace())) {
            tieLen += nel[0].len() * n->chord()->actualTicks().ticks() / NoteEvent::NOTE_LENGTH;
        }
    }

    return tieLen;
}

//---------------------------------------------------------
//   collectNote
//---------------------------------------------------------

static void collectNote(EventsHolder& events, const Note* note, const CollectNoteParams& noteParams, Staff* staff,
                        PitchWheelRenderer& pitchWheelRenderer, const CompatMidiRendererInternal::Context& context)
{
    if (!note->play() || note->hidden()) {      // do not play overlapping notes
        return;
    }
    const bool isInvisibleScoreNote = !note->visible();

    Chord* chord = note->chord();
    bool isGrace = chord->isGrace();
    Chord* sourceChord = chord;
    const Instrument* instr = chord->part()->instrument(chord->tick());
    MidiInstrumentEffect noteEffect = noteParams.effect;

    int noteChannel = getChannel(instr, note, noteEffect, context);
    auto midiEffectFromEvent = [](const NoteEvent& event) {
        if (event.slide()) {
            return MidiInstrumentEffect::SLIDE;
        }

        return MidiInstrumentEffect::NONE;
    };

    int tieLen = calculateTieLength(note);
    if (isGrace) {
        if (CompatMidiRendererInternal::graceNotesMerged(chord)) {
            return;
        }
        chord = toChord(chord->explicitParent());
    }

    int ticks = chord->actualTicks().ticks();   // ticks of the actual note
    bool hasOrnament = false;
    if (!isGrace) {
        if (findFirstTrill(chord)) {
            hasOrnament = true;
        } else {
            for (Articulation* art : chord->articulations()) {
                if (art->isOrnament()) {
                    hasOrnament = true;
                    break;
                }
            }
        }
    }
    bool hasArpeggio = chord->arpeggio() && chord->arpeggio()->playArpeggio();
    const Trill* trill = !isGrace ? findFirstTrill(chord) : nullptr;
    const Ornament* gameplayOrnament = trill ? trill->ornament() : nullptr;
    SymId gameplayOrnamentSymId = trill ? SymId::ornamentTrill : SymId::noSym;
    bool hasGameplayOrnament = trill != nullptr;
    if (!hasGameplayOrnament && !isGrace) {
        for (Articulation* art : chord->articulations()) {
            if (!art->isOrnament() || !isGameplayOrnament(art->symId())) {
                continue;
            }

            hasGameplayOrnament = true;
            gameplayOrnament = toOrnament(art);
            gameplayOrnamentSymId = art->symId();
            break;
        }
    }
    bool applyOrnamentNoteOff = hasOrnament && (!hasArpeggio || hasGameplayOrnament) && !isGrace;
    const bool applyGeneratedPlaybackNoteOff = applyOrnamentNoteOff
                                               || (!isGrace && hasGeneratedGlissandoPlayback(note));
    // calculate additional length due to ties forward
    // taking NoteEvent length adjustments into account

    // Acciaccatura and grace-bend offsets are relative to the parent chord's beat.
    // Their own score ticks are already pre-beat shifted, so using them here would
    // apply the grace shift twice and can merge the grace attack into a previous
    // same-pitch note.
    const bool anchorGraceToParentBeat = isGrace && (noteParams.graceOffsetOn != 0 || noteParams.graceOffsetOff != 0);
    int tick1 = (anchorGraceToParentBeat && chord ? chord->tick().ticks() : sourceChord->tick().ticks())
                + noteParams.tickOffset;
    const GuitarBend* bendFor = note->bendFor();
    const GuitarBend* bendBack = note->bendBack();

    collectPianomaniaHeldPitchCurve(note, noteChannel, tick1, pitchWheelRenderer, noteEffect);

    NoteEventList nel = note->playEvents();
    if (hasGameplayOrnament && applyOrnamentNoteOff) {
        normalizeGameplayOrnamentEvents(nel, note, gameplayOrnament, gameplayOrnamentSymId);
    }
    size_t nels = nel.size();
    const bool applyStaccatoDurationRepair = !isInvisibleScoreNote
                                             && !isGrace
                                             && !hasOrnament
                                             && !hasArpeggio
                                             && chord->playEventType() == PlayEventType::Auto
                                             && nels == 1
                                             && hasOnlyPlainStaccato(chord);
    int mainEventIndex = -1;
    if (applyGeneratedPlaybackNoteOff && nels > 1) {
        mainEventIndex = CompatMidiRendererInternal::canonicalWrittenNoteEventIndex(nel);
    }
    for (size_t i = 0; i < nels; ++i) {
        const NoteEvent& e = nel[i];     // we make an explicit const ref, not a const copy.  no need to copy as we won't change the original object.

        // skip if note has a tie into it and only one NoteEvent
        // its length was already added to previous note
        // if we wish to suppress first note of ornament
        // then change "nels == 1" to "i == 0", and change "break" to "continue"
        if (note->tieBack() && nels == 1 && !isGlissandoFor(note)) {
            Note* tiedBack = note->tieBack()->startNote();
            if (!tiedBack) {
                break;
            }

            const auto& eventsList = tiedBack->playEvents();
            IF_ASSERT_FAILED(!eventsList.empty()) {
                LOGE() << "play events are empty for note on track " << tiedBack->track() << ", tick " << tiedBack->tick().ticks();
                break;
            }

            if (noteEffect == MidiInstrumentEffect::NONE) {
                noteEffect = midiEffectFromEvent(eventsList.front());
                noteChannel = getChannel(instr, note, noteEffect, context);
            }

            break;
        }

        // skipping the notes which are connected by bends
        if (bendBack && bendBack->bendType() != GuitarBendType::PRE_BEND && i == 0) {
            continue;
        }

        int p = std::clamp(note->ppitch() + e.pitch() + note->harmonicPitchOffset(), 0, 127);
        int on = tick1 + (ticks * e.ontime()) / 1000;
        int off = on + (ticks * e.len()) / 1000 - (note->pianomaniaHeldNote() ? 0 : 1);
        if (applyStaccatoDurationRepair && e.play() && e.pitch() == 0 && e.offset() == 0 && e.ontime() == 0) {
            int durationTicks = repairedStaccatoDurationTicks(ticks, off - on);
            if (durationTicks != off - on) {
                off = on + durationTicks;
            }
        }

        if (note->deadNote()) {
            const double ticksPerSecond = chord->score()->multipliedTempo(chord->tick()).val * Constants::DIVISION;
            constexpr double deadNoteDurationInSec = 0.05;
            const double deadNoteDurationInTicks = ticksPerSecond * deadNoteDurationInSec;
            if (off - on > deadNoteDurationInTicks) {
                off = on + deadNoteDurationInTicks;
            }
        } else {
            if ((note->tieFor() || bendFor) && i == nels - 1) {
                off += tieLen;
            }

            if (noteParams.letRingNote && letRingShouldApply(e, note)) {
                off = std::max(off, noteParams.endLetRingTick);
                if (off - on > LET_RING_MAX_TICKS) {
                    off = on + LET_RING_MAX_TICKS;
                }
            }
        }

        // Get the velocity used for this note from the staff
        // This allows correct playback of tremolos even without SND enabled.
        Fraction nonUnwoundTick = Fraction::fromTicks(on - noteParams.tickOffset);
        int velo = context.velocitiesByTrack.at(note->track()).val(nonUnwoundTick) * noteParams.velocityMultiplier * e.velocityMultiplier();
        if (e.play()) {
            PlayNoteParams playParams;
            MidiInstrumentEffect eventEffect = noteEffect;
            int eventChannel = noteChannel;
            playParams.pitch = p;
            playParams.velo = std::clamp(velo, 1, 127);
            playParams.onTime = std::max(0, on - noteParams.graceOffsetOn);
            playParams.offTime = std::max(0, off - noteParams.graceOffsetOff);
            // Pianomania reserves note-off velocity 127 for generated nonvisual playback
            // notes only. Written grace notes are score notes and must keep normal note-off velocity.
            if (!isGrace && isInvisibleScoreNote) {
                playParams.noteOffVelocity = ORNAMENT_NOTE_OFF_VELOCITY;
            } else if (applyGeneratedPlaybackNoteOff && mainEventIndex >= 0 && static_cast<int>(i) != mainEventIndex) {
                playParams.noteOffVelocity = ORNAMENT_NOTE_OFF_VELOCITY;
            }

            if (eventEffect == MidiInstrumentEffect::NONE) {
                eventEffect = midiEffectFromEvent(e);
                eventChannel = getChannel(instr, note, eventEffect, context);
            }

            playParams.effect = eventEffect;
            playParams.channel = eventChannel;

            if (noteParams.graceOffsetOn == 0) {
                playParams.offset = ticks * e.offset() / 1000;
            }

            playParams.staffIdx = static_cast<int>(staff->idx());
            playParams.callAllSoundOff = noteParams.callAllSoundOff;
            playNote(events, note, playParams, pitchWheelRenderer);

            if (instr->singleNoteDynamics()) {
                renderSnd(events, chord, noteChannel, noteParams.tickOffset, context);
            }
        }
    }

    // Bends
    if (bendFor) {
        collectGuitarBend(note, noteChannel, tick1, noteParams.graceOffsetOn, noteParams.previousChordTicks, pitchWheelRenderer,
                          noteEffect);
    } else {
        // old bends implementation
        for (const EngravingItem* e : note->el()) {
            if (!e || (!e->isBend())) {
                continue;
            }

            const Bend* bend = toBend(e);
            if (!bend->playBend()) {
                break;
            }

            collectBend(bend->points(), bend->staffIdx(), noteChannel, tick1, tick1 + getPlayTicksForBend(
                            note).ticks(), pitchWheelRenderer, noteEffect);
        }
    }
}

//---------------------------------------------------------
//   aeolusSetStop
//---------------------------------------------------------

static void aeolusSetStop(int tick, int channel, int i, int k, bool val, EventsHolder& events)
{
    NPlayEvent event;
    event.setType(ME_CONTROLLER);
    event.setController(98);
    if (val) {
        event.setValue(0x40 + 0x20 + i);
    } else {
        event.setValue(0x40 + 0x10 + i);
    }

    event.setChannel(static_cast<uint8_t>(channel));
    events[channel].emplace(tick, event);

    event.setValue(k);
    events[channel].emplace(tick, event);
}

//---------------------------------------------------------
//   collectProgramChanges
//---------------------------------------------------------

static void collectProgramChanges(EventsHolder& events, Measure const* m, const Staff* staff, int tickOffset)
{
    int firstStaffIdx = static_cast<int>(staff->idx());
    int nextStaffIdx  = firstStaffIdx + 1;

    //
    // collect program changes and controller
    //
    for (Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
        for (EngravingItem* e : s->annotations()) {
            if (!e->isStaffTextBase() || static_cast<int>(e->staffIdx()) < firstStaffIdx
                || static_cast<int>(e->staffIdx()) >= nextStaffIdx) {
                continue;
            }
            const StaffTextBase* st1 = toStaffTextBase(e);
            Fraction tick = s->tick() + Fraction::fromTicks(tickOffset);

            Instrument* instr = e->part()->instrument(tick);
            for (const ChannelActions& ca : st1->channelActions()) {
                int channel = instr->channel().at(ca.channel)->channel();
                for (const String& ma : ca.midiActionNames) {
                    NamedEventList* nel = instr->midiAction(ma, ca.channel);
                    if (!nel) {
                        continue;
                    }
                    for (MidiCoreEvent event : nel->events) {
                        event.setChannel(channel);
                        NPlayEvent e1(event);
                        e1.setOriginatingStaff(firstStaffIdx);
                        if (e1.dataA() == CTRL_PROGRAM) {
                            events[channel].emplace(tick.ticks() - 1, e1);
                        } else {
                            events[channel].emplace(tick.ticks(), e1);
                        }
                    }
                }
            }
            if (st1->setAeolusStops()) {
                Staff* s1 = st1->staff();
                int voice   = 0;
                int channel = s1->channel(tick, voice);

                for (int i = 0; i < 4; ++i) {
                    static int num[4] = { 12, 13, 16, 16 };
                    for (int k = 0; k < num[i]; ++k) {
                        aeolusSetStop(tick.ticks(), channel, i, k, st1->getAeolusStop(i, k), events);
                    }
                }
            }
        }
    }
}

//---------------------------------------------------------
//    renderHarmony
///    renders chord symbols
//---------------------------------------------------------
static void renderHarmony(EventsHolder& events, Measure const* m, Harmony* h, int tickOffset,
                          const CompatMidiRendererInternal::Context& context)
{
    if (!h->isRealizable() || context.harmonyChannelSetting == CompatMidiRendererInternal::HarmonyChannelSetting::DISABLED) {
        return;
    }

    if (context.partsWithMutedHarmony.find(h->part()->id().toStdString()) != context.partsWithMutedHarmony.end()) {
        return;
    }

    Staff* staff = m->score()->staff(h->track() / VOICES);
    const InstrChannel* instrChannel = staff->part()->harmonyChannel();
    IF_ASSERT_FAILED(instrChannel) {
        LOGE() << "channel for harmony isn't found for part " << staff->part()->partName();
        return;
    }

    if (!staff->isPrimaryStaff()) {
        return;
    }

    int channel = instrChannel->channel();
    int staffIdx = static_cast<int>(staff->idx());

    if (context.harmonyChannelSetting == CompatMidiRendererInternal::HarmonyChannelSetting::LOOKUP) {
        CompatMidiRendererInternal::ChannelLookup::LookupData lookupData;
        lookupData.harmony = true;
        channel = context.channels->getChannel(channel, lookupData);
    }

    int velocity = context.velocitiesByTrack.at(h->track()).val(h->tick());

    const RealizedHarmony& r = h->getRealizedHarmony();

    NPlayEvent ev(ME_NOTEON, static_cast<uint8_t>(channel), 0, velocity);
    ev.setHarmony(h);
    Fraction duration = r.getActualDuration(h->tick().ticks() + tickOffset);

    int onTime = h->tick().ticks() + tickOffset;
    int offTime = onTime + duration.ticks();

    ev.setOriginatingStaff(staffIdx);
    ev.setTuning(0.0);

    //add play events
    const RealizedHarmony::PitchMap& notes = r.notes();
    for (const auto& [pitch, _] : notes) {
        ev.setPitch(pitch);
        ev.setVelo(velocity);
        events[channel].emplace(onTime, ev);
        NPlayEvent offEv(ME_NOTEOFF, static_cast<uint8_t>(channel), pitch, DEFAULT_NOTE_OFF_VELOCITY);
        offEv.setHarmony(h);
        offEv.setOriginatingStaff(staffIdx);
        offEv.setTuning(0.0);
        events[channel].emplace(offTime, offEv);
    }
}

static ChordRest* findPreviousChordRestForGraceOffset(Chord* chord)
{
    if (!chord || !chord->segment()) {
        return nullptr;
    }

    Segment* previousSegment = chord->segment()->prev1(SegmentType::ChordRest);
    return previousSegment ? previousSegment->nextChordRest(chord->track(), true) : nullptr;
}

void CompatMidiRendererInternal::collectGraceBeforeChordEvents(Chord* chord, Chord* prevChord, EventsHolder& events, double veloMultiplier,
                                                               Staff* st,
                                                               int tickOffset,
                                                               PitchWheelRenderer& pitchWheelRenderer, MidiInstrumentEffect effect)
{
    // calculate offset for grace notes here
    const auto& grChords = chord->graceNotesBefore();
    std::vector<Chord*> graceNotesBeforeBar;
    std::copy_if(grChords.begin(), grChords.end(), std::back_inserter(graceNotesBeforeBar), [](Chord* ch) {
        return ch->noteType() == NoteType::ACCIACCATURA;
    });

    int graceTickSum = 0;
    int graceTickOffset = 0;

    size_t acciacaturaGraceSize = graceNotesBeforeBar.size();
    // prevChords is reset when another voice has a later segment and this voice
    // is empty there. Fall back to the nearest previous chord/rest in this track
    // so measure-boundary acciaccaturas do not share a start tick with the
    // preceding written note.
    ChordRest* previousGraceChordRest = prevChord ? static_cast<ChordRest*>(prevChord)
                                        : findPreviousChordRestForGraceOffset(chord);
    Chord* previousGraceChord = prevChord;
    if (!previousGraceChord && previousGraceChordRest && previousGraceChordRest->isChord()) {
        previousGraceChord = toChord(previousGraceChordRest);
    }

    if (acciacaturaGraceSize > 0) {
        graceTickSum = graceNotesBeforeBar[0]->ticks().ticks();
        if (previousGraceChordRest) {
            int previousTicks = prevChord ? prevChord->ticks().ticks() : previousGraceChordRest->actualTicks().ticks();
            graceTickSum = std::min(previousTicks / 2, graceTickSum);
        }

        graceTickOffset = graceTickSum / static_cast<int>(acciacaturaGraceSize);
    } else {
        bool hasGraceBend = std::any_of(grChords.begin(), grChords.end(), [](Chord* ch) {
            return std::any_of(ch->notes().begin(), ch->notes().end(), [](Note* n) {
                return n->isGraceBendStart();
            });
        });

        if (hasGraceBend) {
            graceTickSum = graceBendDuration(previousGraceChord);
        }
    }

    if (!graceNotesMerged(chord)) {
        int currentBeaforeBeatNote = 0;
        for (Chord* c : grChords) {
            for (const Note* note : c->notes()) {
                GuitarBend* bendFor = note->bendFor();
                if (bendFor && bendFor->bendType() == GuitarBendType::PRE_BEND) {
                    continue;
                }

                CollectNoteParams params;
                params.effect = effect;
                params.velocityMultiplier = veloMultiplier;
                params.tickOffset = tickOffset;

                bool isGraceBend = (note->bendFor() && note->bendFor()->bendType() == GuitarBendType::GRACE_NOTE_BEND);
                if (previousGraceChordRest) {
                    params.previousChordTicks = previousGraceChordRest->actualTicks().ticks();
                }

                if (note->noteType() == NoteType::ACCIACCATURA) {
                    params.graceOffsetOn = graceTickSum - graceTickOffset * currentBeaforeBeatNote;
                    params.graceOffsetOff = graceTickSum - graceTickOffset * (currentBeaforeBeatNote + 1);

                    collectNote(events, note, params, st, pitchWheelRenderer, m_context);
                } else if (note->noteType() == NoteType::APPOGGIATURA) {
                    collectNote(events, note, params, st, pitchWheelRenderer, m_context);
                } else if (isGraceBend) {
                    params.graceOffsetOn = graceTickSum;
                    params.graceOffsetOff = 0;

                    collectNote(events, note, params, st, pitchWheelRenderer, m_context);
                } else {
                    collectNote(events, note, params, st, pitchWheelRenderer, m_context);
                }
            }

            currentBeaforeBeatNote++;
        }
    }
}

bool shouldPlayHammerOn(const Chord* chord)
{
    int currentTick = chord->tick().ticks();
    Chord* firstTiedChord = chord->nextTiedChord(true, false);
    const Score* score = chord->score();
    while (firstTiedChord) {
        currentTick = firstTiedChord->tick().ticks();
        firstTiedChord = firstTiedChord->nextTiedChord(true, false);
    }

    for (auto it : score->spannerMap().findOverlapping(currentTick, currentTick + chord->ticks().ticks())) {
        Spanner* spanner = it.value;
        if (spanner->track() != chord->track()) {
            continue;
        }

        if (spanner->isHammerOnPullOff() && (spanner->startChord() != chord)) {
            return true;
        }
    }

    return false;
}

static void collectTurnBetweenNotes(EventsHolder& events, Trill* trill, Chord* startChord, Staff* staff,
                                    double veloMultiplier, int tickOffset, PitchWheelRenderer& pitchWheelRenderer,
                                    MidiInstrumentEffect effect, const CompatMidiRendererInternal::Context& context)
{
    if (!trill || !startChord || !staff) {
        return;
    }

    if (trill->trillType() != TrillType::TURN_BETWEEN || !trill->playSpanner()) {
        return;
    }

    if (trill->track() != startChord->track()) {
        return;
    }

    EngravingItem* endElement = trill->endElement();
    if (!endElement || !endElement->isChord()) {
        return;
    }

    Chord* endChord = toChord(endElement);
    int startTick = startChord->tick().ticks() + tickOffset;
    int endTick = endChord->tick().ticks() + tickOffset;
    if (endTick <= startTick || startChord->notes().empty()) {
        return;
    }

    Note* note = startChord->notes().front();
    NoteEventList turnEvents;
    if (!CompatMidiRender::renderNoteArticulation(&turnEvents, note, false, trill->trillType(), trill->ornamentStyle(), 0,
                                                  trill->ornament())) {
        return;
    }

    int windowTicks = endTick - startTick;
    int turnTicks = std::max(1, windowTicks / 2);
    int turnStartTick = endTick - turnTicks;

    const Instrument* instr = startChord->part()->instrument(startChord->tick());
    int noteChannel = getChannel(instr, note, effect, context);

    for (const NoteEvent& e : turnEvents) {
        if (!e.play()) {
            continue;
        }

        int p = std::clamp(note->ppitch() + e.pitch(), 0, 127);
        int on = turnStartTick + (turnTicks * e.ontime()) / NoteEvent::NOTE_LENGTH;
        int off = on + (turnTicks * e.len()) / NoteEvent::NOTE_LENGTH - 1;

        Fraction nonUnwoundTick = Fraction::fromTicks(on - tickOffset);
        int velo = context.velocitiesByTrack.at(note->track()).val(nonUnwoundTick) * veloMultiplier * e.velocityMultiplier();

        PlayNoteParams playParams;
        playParams.channel = noteChannel;
        playParams.pitch = p;
        playParams.velo = std::clamp(velo, 1, 127);
        playParams.noteOffVelocity = ORNAMENT_NOTE_OFF_VELOCITY;
        playParams.onTime = std::max(0, on);
        playParams.offTime = std::max(0, off);
        playParams.offset = 0;
        playParams.staffIdx = static_cast<int>(staff->idx());
        playParams.effect = effect;
        if (context.eachStringHasChannel && instr->hasStrings()) {
            playParams.callAllSoundOff = true;
        }

        playNote(events, note, playParams, pitchWheelRenderer);
    }
}

CompatMidiRendererInternal::ChordParams CompatMidiRendererInternal::collectChordParams(const Chord* chord, int tickOffset) const
{
    ChordParams chordParams;

    int currentTick = chord->tick().ticks();
    for (auto it : score->spannerMap().findOverlapping(currentTick + 1, currentTick + 2)) {
        Spanner* spanner = it.value;
        if (spanner->track() != chord->track()) {
            continue;
        }

        if (spanner->isLetRing()) {
            LetRing* letRing = toLetRing(spanner);
            chordParams.letRing = true;
            ChordRest* endCR = letRing->endCR();
            chordParams.endLetRingTick = (endCR ? endCR->endTick().ticks() : letRing->tick2().ticks()) + tickOffset;
        } else if (spanner->isPalmMute()) {
            chordParams.palmMute = true;
        }
    }

    chordParams.hammerOnPullOff = shouldPlayHammerOn(chord);
    return chordParams;
}

//---------------------------------------------------------
//   doCollectMeasureEvents
//---------------------------------------------------------

void CompatMidiRendererInternal::doCollectMeasureEvents(EventsHolder& events, Measure const* m, const Staff* staff, int tickOffset,
                                                        PitchWheelRenderer& pitchWheelRenderer, std::array<Chord*, VOICES>& prevChords)
{
    staff_idx_t firstStaffIdx = staff->idx();
    for (Staff* st : staff->masterScore()->staves()) {
        if (staff->id() == st->id()) {
            firstStaffIdx = st->idx();
        }
    }
    staff_idx_t nextStaffIdx  = firstStaffIdx + 1;

    SegmentType st = SegmentType::ChordRest;
    track_idx_t strack = firstStaffIdx * VOICES;
    track_idx_t etrack = nextStaffIdx * VOICES;
    for (Segment* seg = m->first(st); seg; seg = seg->next(st)) {
        Fraction tick = seg->tick();

        //render harmony
        for (EngravingItem* e : seg->annotations()) {
            if (!e || (e->track() < strack) || (e->track() >= etrack)) {
                continue;
            }
            Harmony* h = nullptr;
            if (e->isHarmony()) {
                h = toHarmony(e);
            } else if (e->isFretDiagram()) {
                h = toFretDiagram(e)->harmony();
            }
            if (!h || !h->play()) {
                continue;
            }
            renderHarmony(events, m, h, tickOffset, m_context);
        }

        for (track_idx_t track = strack; track < etrack; ++track) {
            // Skip linked staves, except primary
            Staff* st1 = m->score()->staff(track / VOICES);
            if (!st1->isPrimaryStaff()) {
                track += VOICES - 1;
                continue;
            }

            size_t voice = track % VOICES;
            EngravingItem* cr = seg->element(track);
            if (!cr || !cr->isChord()) {
                prevChords[voice] = nullptr;
                continue;
            }

            Chord* chord = toChord(cr);
            if (chord->isGrace()) {
                // Grace notes are collected via the parent chord's grace-before/after paths.
                continue;
            }
            double veloMultiplier = NoteEvent::DEFAULT_VELOCITY_MULTIPLIER * chordVelocityMultiplier(chord, m_context);

            //
            // Add normal note events
            //
            ChordParams chordParams = collectChordParams(chord, tickOffset);

            MidiInstrumentEffect effect = MidiInstrumentEffect::NONE;
            if (chordParams.palmMute) {
                effect = MidiInstrumentEffect::PALM_MUTE;
            } else if (chordParams.hammerOnPullOff) {
                effect = MidiInstrumentEffect::HAMMER_PULL;
            }

            collectGraceBeforeChordEvents(chord, prevChords[voice], events, veloMultiplier, st1, tickOffset, pitchWheelRenderer, effect);

            Instrument* instr = st1->part()->instrument(tick);
            for (const Note* note : chord->notes()) {
                CollectNoteParams params;
                params.velocityMultiplier = veloMultiplier;
                params.tickOffset = tickOffset;
                params.letRingNote = chordParams.letRing;
                params.endLetRingTick = chordParams.endLetRingTick;
                if (m_context.instrumentsHaveEffects) {
                    params.effect = effect;
                }

                if (m_context.eachStringHasChannel && instr->hasStrings()) {
                    params.callAllSoundOff = true;
                }

                collectNote(events, note, params, st1, pitchWheelRenderer, m_context);
            }

            if (!graceNotesMerged(chord)) {
                for (Chord* c : chord->graceNotesAfter()) {
                    for (const Note* note : c->notes()) {
                        CollectNoteParams params;
                        params.velocityMultiplier = veloMultiplier;
                        params.tickOffset = tickOffset;
                        params.effect = effect;
                        collectNote(events, note, params, st1, pitchWheelRenderer, m_context);
                    }
                }
            }

            for (Spanner* spanner : chord->startingSpanners()) {
                if (!spanner->isTrill()) {
                    continue;
                }
                collectTurnBetweenNotes(events, toTrill(spanner), chord, st1, veloMultiplier, tickOffset,
                                        pitchWheelRenderer, effect, m_context);
            }

            prevChords[voice] = chord;
        }
    }
}

int CompatMidiRendererInternal::canonicalWrittenNoteEventIndex(const NoteEventList& events)
{
    int result = -1;
    int bestOntime = std::numeric_limits<int>::max();
    for (size_t i = 0; i < events.size(); ++i) {
        if (!events[i].play() || events[i].pitch() != 0) {
            continue;
        }

        const int ontime = events[i].ontime();
        if (ontime < bestOntime) {
            bestOntime = ontime;
            result = static_cast<int>(i);
        }
    }
    return result;
}

CompatMidiRendererInternal::CompatMidiRendererInternal(Score* s)
    : score(s)
{
}

//---------------------------------------------------------
//   collectMeasureEvents
//    redirects to the correct function based on the passed method
//---------------------------------------------------------

void CompatMidiRendererInternal::collectMeasureEvents(EventsHolder& events, Measure const* m, const Staff* staff, int tickOffset,
                                                      PitchWheelRenderer& pitchWheelRenderer, std::array<Chord*, VOICES>& prevChords)
{
    doCollectMeasureEvents(events, m, staff, tickOffset, pitchWheelRenderer, prevChords);

    collectProgramChanges(events, m, staff, tickOffset);
}

//---------------------------------------------------------
//   renderStaff
//---------------------------------------------------------

void CompatMidiRendererInternal::renderStaff(EventsHolder& events, const Staff* staff, PitchWheelRenderer& pitchWheelRenderer)
{
    Measure const* lastMeasure = nullptr;

    const RepeatList& repeatList = score->repeatList();
    std::array<Chord*, VOICES> prevChords = { nullptr };

    for (const RepeatSegment* rs : repeatList) {
        const int tickOffset = rs->utick - rs->tick;

        Measure const* const start = rs->firstMeasure();

        for (Measure const* m = start; m; m = m->nextMeasure()) {
            staff_idx_t staffIdx = staff->idx();
            if (m->isMeasureRepeatGroup(staffIdx)) {
                MeasureRepeat* mr = m->measureRepeatElement(staffIdx);
                Measure const* playMeasure = lastMeasure;
                if (!playMeasure || !mr) {
                    continue;
                }

                for (int i = m->measureRepeatCount(staffIdx); i < mr->numMeasures() && playMeasure->prevMeasure(); ++i) {
                    playMeasure = playMeasure->prevMeasure();
                }

                int offset = (m->tick() - playMeasure->tick()).ticks();
                collectMeasureEvents(events, playMeasure, staff, tickOffset + offset, pitchWheelRenderer, prevChords);
            } else {
                lastMeasure = m;
                collectMeasureEvents(events, lastMeasure, staff, tickOffset, pitchWheelRenderer, prevChords);
            }

            if (m == rs->lastMeasure()) {
                break;
            }
        }
    }
}

//---------------------------------------------------------
//   renderSpanners
//---------------------------------------------------------

void CompatMidiRendererInternal::renderSpanners(EventsHolder& events, PitchWheelRenderer& pitchWheelRenderer)
{
    for (const auto& sp : score->spannerMap().map()) {
        Spanner* s = sp.second;

        if (!s->staff()->isPrimaryStaff()) {
            continue;
        }

        int idx = s->staff()->channel(s->tick(), 0);
        int channel = s->part()->instrument(s->tick())->channel(idx)->channel();
        const auto& channels = m_context.channels->channelsMap[channel];
        if (channels.empty()) {
            doRenderSpanners(events, s, channel, pitchWheelRenderer, MidiInstrumentEffect::NONE);
        } else {
            for (const auto& channel2 : channels) {
                doRenderSpanners(events, s, channel2.second, pitchWheelRenderer, channel2.first.effect);
            }
        }
    }
}

static std::vector<std::pair<int, int> > collectTicksForEffect(const Score* const score, track_idx_t track, int stick, int etick,
                                                               MidiInstrumentEffect effect)
{
    std::vector<std::pair<int, int> > ticksForEffect;
    int curTick = stick;

    for (auto it : score->spannerMap().findOverlapping(stick, etick)) {
        Spanner* spanner = it.value;

        if (spanner->track() != track) {
            continue;
        }

        if (spanner->isPalmMute()) {
            int palmMuteStartTick = spanner->tick().ticks();
            int palmMuteEndTick = spanner->tick2().ticks();

            if (curTick < palmMuteStartTick) {
                int nextTick = std::min(palmMuteStartTick, etick);
                if (effect == MidiInstrumentEffect::NONE) {
                    ticksForEffect.push_back({ curTick, nextTick - 1 });
                }

                curTick = nextTick;
            }

            if (palmMuteStartTick <= curTick) {
                int nextTick = std::min(palmMuteEndTick, etick);
                if (effect == MidiInstrumentEffect::PALM_MUTE) {
                    ticksForEffect.push_back({ curTick, nextTick - 1 });
                }

                curTick = nextTick;
            }
        }
    }

    if (curTick < etick && effect == MidiInstrumentEffect::NONE) {
        ticksForEffect.push_back({ curTick, etick - 1 });
    }

    return ticksForEffect;
}

static VibratoParams getVibratoParams(VibratoType type)
{
    VibratoParams params;

    switch (type) {
    case VibratoType::GUITAR_VIBRATO:
        // guitar vibrato, up only
        params.lowPitch = 0;
        params.highPitch = 10;
        params.period = Constants::DIVISION / 3;
        break;

    case VibratoType::GUITAR_VIBRATO_WIDE:
        params.lowPitch = 0;         // 100 is a semitone
        params.highPitch = 20;
        params.period = Constants::DIVISION / 2.5;
        break;

    case VibratoType::VIBRATO_SAWTOOTH_WIDE:
        // vibrato with whammy bar up and down
        params.lowPitch = -25;         // 1/16
        params.highPitch = 25;
        params.period = Constants::DIVISION / 2;
        break;

    case VibratoType::VIBRATO_SAWTOOTH:
        params.lowPitch = -12;
        params.highPitch = 12;
        params.period = Constants::DIVISION / 2;
        break;

    default:
        LOGE() << "vibrato type is not handled in midi renderer";
        break;
    }

    return params;
}

void CompatMidiRendererInternal::doRenderSpanners(EventsHolder& events, Spanner* s, uint32_t channel,
                                                  PitchWheelRenderer& pitchWheelRenderer,
                                                  MidiInstrumentEffect effect)
{
    struct PedalEvent {
        int tick = 0;
        bool on = true;
        int staffIdx = 0;

        PedalEvent() = default;
        PedalEvent(int tick, bool on, int staffIdx)
            : tick(tick), on(on), staffIdx(staffIdx)
        {
        }
    };

    std::vector<PedalEvent> pedalEventList;

    int staffIdx = static_cast<int>(s->staffIdx());

    if (s->isPedal()) {
        PedalEvent lastEvent;

        if (!pedalEventList.empty()) {
            lastEvent = pedalEventList.back();
        } else {
            lastEvent = { 0, true, staffIdx };
        }

        int st = s->tick().ticks();

        if (!lastEvent.on && lastEvent.tick >= (st + 2)) {
            pedalEventList.emplace(pedalEventList.cend() - 1,
                                   st + (2 - MScore::pedalEventsMinTicks), false, staffIdx);
        }
        int a = st + 2;
        pedalEventList.emplace_back(a, true, staffIdx);

        int t = s->tick2().ticks() + (2 - MScore::pedalEventsMinTicks);
        if (!score->repeatList().empty()) {
            const RepeatSegment& lastRepeat = *score->repeatList().back();
            if (t > lastRepeat.utick + lastRepeat.len()) {
                t = lastRepeat.utick + lastRepeat.len();
            }
        }
        pedalEventList.emplace_back(t, false, staffIdx);
    } else if (s->isVibrato()) {
        int stick = s->tick().ticks();
        int etick = s->tick2().ticks();

        // from start to end of trill, send bend events at regular interval
        Vibrato* t = toVibrato(s);
        VibratoParams vibratoParams = getVibratoParams(t->vibratoType());

        std::vector<std::pair<int, int> > vibratoTicksForEffect = collectTicksForEffect(score, s->track(), stick, etick, effect);

        for (const auto& [tickStart, tickEnd] : vibratoTicksForEffect) {
            collectVibrato(channel, tickStart, tickEnd, vibratoParams, pitchWheelRenderer, effect, s->staffIdx());
        }
    }

    for (const auto& pe : pedalEventList) {
        NPlayEvent event;
        if (pe.on) {
            event = NPlayEvent(ME_CONTROLLER, static_cast<uint8_t>(channel), CTRL_SUSTAIN, 127);
        } else {
            event = NPlayEvent(ME_CONTROLLER, static_cast<uint8_t>(channel), CTRL_SUSTAIN, 0);
        }
        event.setOriginatingStaff(pe.staffIdx);
        event.setEffect(effect);
        events[channel].emplace(pe.tick, event);
    }
}

//---------------------------------------------------------
// findFirstTrill
//  search the spanners in the score, finding the first one
//  which overlaps this chord and is of type ElementType::TRILL
//---------------------------------------------------------

static Trill* findFirstTrill(Chord* chord)
{
    auto spanners = chord->score()->spannerMap().findOverlapping(1 + chord->tick().ticks(), chord->endTick().ticks() - 1);
    for (auto i : spanners) {
        if (!i.value->isTrill()) {
            continue;
        }
        if (i.value->track() != chord->track()) {
            continue;
        }
        Trill* trill = toTrill(i.value);
        if (trill->trillType() == TrillType::TURN_BETWEEN) {
            continue;
        }
        if (!trill->playSpanner()) {
            continue;
        }
        return trill;
    }
    return nullptr;
}

void CompatMidiRendererInternal::renderScore(EventsHolder& events, const Context& context, bool expandRepeats)
{
    UNUSED(expandRepeats);

    m_context = context;
    PitchWheelRenderer pitchWheelRender(g_wheelSpec);

    score->updateSwing();
    score->updateCapo();

    if (!m_context.useDefaultArticulations) {
        fillArticulationsInfo();
    }

    CompatMidiRender::createPlayEvents(score, score->firstMeasure(), nullptr, m_context);

    score->updateChannel();
    fillScoreVelocities(score, m_context);

    // create note & other events
    for (const Staff* st : score->staves()) {
        renderStaff(events, st, pitchWheelRender);
    }
    events.fixupMIDI();

    // create sustain pedal events
    renderSpanners(events, pitchWheelRender);

    EventsHolder pitchWheelEvents = pitchWheelRender.renderPitchWheel();
    events.mergePitchWheelEvents(pitchWheelEvents);
    if (m_context.applyCaesuras) {
        m_context.pauseMap->calculate(score);
    }
}

void CompatMidiRendererInternal::fillArticulationsInfo()
{
    for (const Part* part : score->parts()) {
        for (const auto& [tick, instr] : part->instruments()) {
            String instrId = instr->id();
            for (auto it = Context::s_builtInArticulationsValues.cbegin(); it != Context::s_builtInArticulationsValues.cend(); it++) {
                const String& articulationName = it->first;
                const std::vector<MidiArticulation>& instrArticulations = instr->articulation();
                bool instrHasArticulation
                    = std::any_of(instrArticulations.begin(),
                                  instrArticulations.end(), [articulationName](const MidiArticulation& instrArticulation) {
                    return instrArticulation.name == articulationName;
                });

                if (!instrHasArticulation) {
                    m_context.articulationsWithoutValuesByInstrument[instrId].insert(articulationName);
                }
            }
        }
    }
}

double chordVelocityMultiplier(const Chord* chord, const CompatMidiRendererInternal::Context& context)
{
    double veloMultiplier = 1.0;
    Instrument* instr = chord->part()->instrument();
    for (Articulation* a : chord->articulations()) {
        if (a->playArticulation()) {
            veloMultiplier *= velocityMultiplierByInstrument(instr, a->articulationName(), context);
        }
    }

    return veloMultiplier;
}

double velocityMultiplierByInstrument(const Instrument* instrument, const String& articulationName,
                                      const CompatMidiRendererInternal::Context& context)
{
    using Ctx = CompatMidiRendererInternal::Context;
    if (context.useDefaultArticulations) {
        auto it = Ctx::s_builtInArticulationsValues.find(articulationName);
        if (it != Ctx::s_builtInArticulationsValues.end()) {
            return it->second.velocityMultiplier;
        }
    } else {
        auto articulationsForInstrumentIt = context.articulationsWithoutValuesByInstrument.find(instrument->id());
        if (articulationsForInstrumentIt != context.articulationsWithoutValuesByInstrument.end()) {
            const auto& articulationsForInstrument = articulationsForInstrumentIt->second;
            if (articulationsForInstrument.find(articulationName) != articulationsForInstrument.end()) {
                return Ctx::s_builtInArticulationsValues[articulationName].velocityMultiplier;
            }
        }
    }

    return instrument->getVelocityMultiplier(articulationName);
}

/* static */
uint32_t getChannel(const Instrument* instr, const Note* note, MidiInstrumentEffect effect,
                    const CompatMidiRendererInternal::Context& context)
{
    int subchannel = note->subchannel();
    int channel = instr->channel(subchannel)->channel();

    if (!context.instrumentsHaveEffects && !context.eachStringHasChannel) {
        return channel;
    }

    CompatMidiRendererInternal::ChannelLookup::LookupData lookupData;

    if (context.instrumentsHaveEffects) {
        lookupData.effect = effect;
    }

    if (context.eachStringHasChannel && instr->hasStrings()) {
        if (note->string() >= 0) {
            lookupData.string = note->string();
        } else {
            int string = 0;
            int fret = 0;
            const StringData* stringData = instr->stringData();
            IF_ASSERT_FAILED(stringData && stringData->convertPitch(note->pitch(), note->staff(), &string, &fret)) {
                LOGE() << "channel isn't calculated for instrument " << instr->nameAsPlainText();
                return channel;
            }

            lookupData.string = string;
        }

        lookupData.staffIdx = note->staffIdx();
    }

    return context.channels->getChannel(channel, lookupData);
}

uint32_t CompatMidiRendererInternal::ChannelLookup::getChannel(uint32_t instrumentChannel, const LookupData& lookupData)
{
    auto& channelsForInstrument = channelsMap[instrumentChannel];

    auto channelIt = channelsForInstrument.find(lookupData);
    if (channelIt != channelsForInstrument.end()) {
        return channelIt->second;
    }

    channelsForInstrument.insert({ lookupData, maxChannel });
    return maxChannel++;
}

bool CompatMidiRendererInternal::ChannelLookup::LookupData::operator<(const CompatMidiRendererInternal::ChannelLookup::LookupData& other)
const
{
    if (harmony && !other.harmony) {
        return true;
    }

    if (!harmony && other.harmony) {
        return false;
    }

    return std::tie(string, staffIdx, effect) < std::tie(other.string, other.staffIdx, other.effect);
}

// In the case that a chord has an ornament without written grace notes, playback
// can synthesize the ornamental motion from the main chord's articulation events.
// Written grace notes remain score notes and must be exported through the grace
// chord paths so Pianomania receives their MIDI note events.

bool CompatMidiRendererInternal::graceNotesMerged(Chord* chord)
{
    if (chord->isGrace()) {
        return false;
    }

    const auto& graceBefore = chord->graceNotesBefore(true);
    const auto& graceAfter = chord->graceNotesAfter(true);
    if (!graceBefore.empty() || !graceAfter.empty()) {
        return false;
    }
    if (findFirstTrill(chord)) {
        return true;
    }
    for (Articulation* a : chord->articulations()) {
        for (auto& oe : excursions) {
            if (oe.atype == a->symId()) {
                return true;
            }
        }
    }
    return false;
}

/* static */
void fillHairpinVelocities(const Hairpin* h, std::unordered_map<track_idx_t, VelocityMap>& velocitiesByTrack)
{
    Staff* st = h->staff();
    Fraction tick  = h->tick();
    Fraction tick2 = h->tick2();
    int veloChange  = h->veloChange();
    ChangeMethod method = h->veloChangeMethod();

    // Make the change negative when the hairpin is a diminuendo
    HairpinType htype = h->hairpinType();
    ChangeDirection direction = ChangeDirection::INCREASING;
    if (htype == HairpinType::DIM_HAIRPIN || htype == HairpinType::DIM_LINE) {
        veloChange *= -1;
        direction = ChangeDirection::DECREASING;
    }

    switch (h->voiceAssignment()) {
    case VoiceAssignment::ALL_VOICE_IN_STAFF:
        if (st->isPrimaryStaff()) {
            for (track_idx_t track = st->idx() * VOICES; track < (st->idx() + 1) * VOICES; ++track) {
                velocitiesByTrack[track].addHairpin(tick, tick2, veloChange, method, direction);
            }
        }
        break;
    case VoiceAssignment::ALL_VOICE_IN_INSTRUMENT:
        for (Staff* s : st->part()->staves()) {
            if (!s->isPrimaryStaff()) {
                continue;
            }
            for (track_idx_t track = s->idx() * VOICES; track < (s->idx() + 1) * VOICES; ++track) {
                velocitiesByTrack[track].addHairpin(tick, tick2, veloChange, method, direction);
            }
        }
        break;
    case VoiceAssignment::CURRENT_VOICE_ONLY:
        if (st->isPrimaryStaff()) {
            velocitiesByTrack[h->track()].addHairpin(tick, tick2, veloChange, method, direction);
        }
        break;
    }
}

void fillScoreVelocities(const Score* score, CompatMidiRendererInternal::Context& context)
{
    Score* mainScore = score->masterScore();

    if (!mainScore->firstMeasure()) {
        return;
    }

    for (Segment* s = mainScore->firstMeasure()->first(); s; s = s->next1()) {
        Fraction tick = s->tick();
        for (const EngravingItem* e : s->annotations()) {
            if (!e->isDynamic() || !e->staff()->isPrimaryStaff()) {
                continue;
            }

            const Dynamic* d = toDynamic(e);
            int v = d->velocity();

            // treat an invalid dynamic as no change, i.e. a dynamic set to 0
            if (v < 1) {
                continue;
            }

            // make sure value is legal
            v = std::clamp(v, 1, 127);

            // If a dynamic has 'velocity change' update its ending
            int change = d->changeInVelocity();
            ChangeDirection direction = ChangeDirection::INCREASING;
            if (change < 0) {
                direction = ChangeDirection::DECREASING;
            }

            switch (d->voiceAssignment()) {
            case VoiceAssignment::ALL_VOICE_IN_STAFF: {
                for (track_idx_t track = d->staffIdx() * VOICES; track < (d->staffIdx() + 1) * VOICES; ++track) {
                    context.velocitiesByTrack[track].addDynamic(tick, v);
                }
                if (change != 0) {
                    Fraction etick = tick + d->velocityChangeLength();
                    ChangeMethod method = ChangeMethod::NORMAL;
                    for (track_idx_t track = d->staffIdx() * VOICES; track < (d->staffIdx() + 1) * VOICES; ++track) {
                        context.velocitiesByTrack[track].addHairpin(tick, etick, change, method, direction);
                    }
                }
            }
            break;
            case VoiceAssignment::ALL_VOICE_IN_INSTRUMENT: {
                Part* part = d->staff()->part();
                staff_idx_t pStartStaff = part->staves().front()->idx();
                staff_idx_t pEndStaff = part->staves().back()->idx();
                if (d->staffIdx() < pStartStaff || d->staffIdx() > pEndStaff) {
                    break;
                }
                for (staff_idx_t staffIdx = pStartStaff; staffIdx <= pEndStaff; ++staffIdx) {
                    Staff* stp = mainScore->staff(staffIdx);
                    if (!stp->isPrimaryStaff()) {
                        continue;
                    }
                    for (track_idx_t track = stp->idx() * VOICES; track < (stp->idx() + 1) * VOICES; ++track) {
                        context.velocitiesByTrack[track].addDynamic(tick, v);
                    }
                    if (change != 0) {
                        Fraction etick = tick + d->velocityChangeLength();
                        ChangeMethod method = ChangeMethod::NORMAL;
                        for (track_idx_t track = stp->idx() * VOICES; track < (stp->idx() + 1) * VOICES; ++track) {
                            context.velocitiesByTrack[track].addHairpin(tick, etick, change, method, direction);
                        }
                    }
                }
            }
            break;
            case VoiceAssignment::CURRENT_VOICE_ONLY: {
                context.velocitiesByTrack[d->track()].addDynamic(tick, v);
                if (change != 0) {
                    Fraction etick = tick + d->velocityChangeLength();
                    ChangeMethod method = ChangeMethod::NORMAL;
                    context.velocitiesByTrack[d->track()].addHairpin(tick, etick, change, method, direction);
                }
            }
            break;
            }
        }

        if (s->isChordRestType()) {
            for (track_idx_t track = 0; track < mainScore->ntracks(); ++track) {
                EngravingItem* el = s->element(track);
                if (!el || !el->isChord() || !el->staff()->isPrimaryStaff()) {
                    continue;
                }

                Chord* chord = toChord(el);

                double veloMultiplier = chordVelocityMultiplier(chord, context);

                if (muse::RealIsEqual(veloMultiplier, 1.0)) {
                    continue;
                }

                Fraction ARTICULATION_CHANGE_TIME = std::min(s->ticks(), ARTICULATION_CHANGE_TIME_MAX);
                int start = veloMultiplier * CompatMidiRendererInternal::ARTICULATION_CONV_FACTOR;
                int change = (veloMultiplier - 1) * CompatMidiRendererInternal::ARTICULATION_CONV_FACTOR;
                context.velocityMultiplicationsByTrack[track].addDynamic(chord->tick(), start);
                context.velocityMultiplicationsByTrack[track].addHairpin(chord->tick(),
                                                                         chord->tick() + ARTICULATION_CHANGE_TIME, change, ChangeMethod::NORMAL,
                                                                         ChangeDirection::DECREASING);
            }
        }
    }

    for (const auto& sp : mainScore->spannerMap().map()) {
        Spanner* s = sp.second;
        if (!s->isHairpin() || !s->staff()->isPrimaryStaff()) {
            continue;
        }

        fillHairpinVelocities(toHairpin(s), context.velocitiesByTrack);
    }

    for (Staff* st : mainScore->staves()) {
        if (!st->isPrimaryStaff()) {
            continue;
        }
        for (track_idx_t track = st->idx() * VOICES; track < (st->idx() + 1) * VOICES; ++track) {
            context.velocitiesByTrack[track].setup();
            context.velocityMultiplicationsByTrack[track].setup();
        }
    }

    for (auto it = mainScore->spanner().cbegin(); it != mainScore->spanner().cend(); ++it) {
        Spanner* spanner = (*it).second;
        if (!spanner->isVolta()) {
            continue;
        }

        Volta* volta = toVolta(spanner);
        Staff* st = volta->staff();
        if (!st->isPrimaryStaff()) {
            continue;
        }
        for (track_idx_t track = st->idx() * VOICES; track < (st->idx() + 1) * VOICES; ++track) {
            fillVoltaVelocities(volta, context.velocitiesByTrack[track]);
        }
    }
}

/* static */
void fillVoltaVelocities(const Volta* volta, VelocityMap& veloMap)
{
    Measure* startMeasure = volta->startMeasure();
    Measure* endMeasure = volta->endMeasure();

    if (startMeasure && endMeasure) {
        if (!endMeasure->repeatEnd()) {
            return;
        }

        Fraction startTick = startMeasure->tick() - Fraction::eps();
        Fraction endTick = endMeasure->endTick() - Fraction::eps();
        int prevVelo = veloMap.val(startTick);
        veloMap.addDynamic(endTick, prevVelo);
    }
}
}
