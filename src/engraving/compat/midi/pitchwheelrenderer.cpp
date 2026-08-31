#include "pitchwheelrenderer.h"

#include <algorithm>
#include <stdexcept>

#include "log.h"

using namespace mu::engraving;

PitchWheelRenderer::PitchWheelRenderer(PitchWheelSpecs wheelSpec)
    : _wheelSpec(wheelSpec)
{}

void PitchWheelRenderer::addPitchWheelFunction(const PitchWheelFunction& function, uint32_t channel, staff_idx_t staffIdx,
                                               MidiInstrumentEffect effect)
{
    for (const PianomaniaPitchWheelRange& range : _pianomaniaRanges[channel]) {
        if (function.mStartTick < range.endTick && range.startTick < function.mEndTick) {
            throw std::runtime_error("Pianomania held-note pitch curve overlaps another pitch-wheel source on one MIDI channel");
        }
    }
    _otherRanges[channel].emplace_back(function.mStartTick, function.mEndTick);
    PitchWheelFunctions& functions =  _functions[channel];
    _effectByChannel[channel] = effect;
    _staffIdxByChannel[channel] = staffIdx;

    if (function.mStartTick < functions.startTick) {
        functions.startTick = function.mStartTick;
    }
    if (function.mEndTick > functions.endTick) {
        functions.endTick = function.mEndTick;
    }

    functions.functions.push_back(function);
}

void PitchWheelRenderer::addPianomaniaPitchWheelFunction(const PitchWheelFunction& function, uint32_t channel,
                                                         staff_idx_t staffIdx, MidiInstrumentEffect effect,
                                                         std::string identity, int sensitivitySemitones)
{
    for (const auto& range : _otherRanges[channel]) {
        if (function.mStartTick < range.second && range.first < function.mEndTick) {
            throw std::runtime_error("Pianomania held-note pitch curve overlaps another pitch-wheel source on one MIDI channel");
        }
    }
    for (const PianomaniaPitchWheelRange& range : _pianomaniaRanges[channel]) {
        if (function.mStartTick >= range.endTick || range.startTick >= function.mEndTick) {
            continue;
        }
        if (function.mStartTick == range.startTick && function.mEndTick == range.endTick
            && identity == range.identity && sensitivitySemitones == range.sensitivitySemitones) {
            return;
        }
        throw std::runtime_error("Conflicting simultaneous Pianomania held-note pitch targets share one MIDI channel");
    }

    _pianomaniaRanges[channel].push_back({ function.mStartTick, function.mEndTick, std::move(identity), sensitivitySemitones });
    PitchWheelFunctions& functions = _functions[channel];
    _effectByChannel[channel] = effect;
    _staffIdxByChannel[channel] = staffIdx;
    functions.startTick = std::min(functions.startTick, function.mStartTick);
    functions.endTick = std::max(functions.endTick, function.mEndTick);
    functions.functions.push_back(function);
}

EventsHolder PitchWheelRenderer::renderPitchWheel() const noexcept
{
    EventsHolder pitchWheelEvents;

    for (const auto& function : _functions) {
        renderChannelPitchWheel(pitchWheelEvents, function.second, function.first);
    }

    return pitchWheelEvents;
}

//! MARK: PRIVATE METHODS

void PitchWheelRenderer::renderChannelPitchWheel(EventsHolder& pitchWheelEvents,
                                                 const PitchWheelFunctions& functions,
                                                 uint32_t channel) const noexcept
{
    if (functions.endTick < functions.startTick) {
        return;
    }

    MidiInstrumentEffect effect = MidiInstrumentEffect::NONE;
    if (_effectByChannel.find(channel) != _effectByChannel.end()) {
        effect = _effectByChannel.at(channel);
    }

    bool staffInfoValid = false;
    staff_idx_t staffIdx = 0;

    if (_staffIdxByChannel.find(channel) != _staffIdxByChannel.end()) {
        staffInfoValid = true;
        staffIdx = _staffIdxByChannel.at(channel);
    }

    auto pianomaniaIt = _pianomaniaRanges.find(channel);
    auto addController = [&](int tick, int controller, int value) {
        NPlayEvent event(ME_CONTROLLER, channel, controller, value);
        if (staffInfoValid) {
            event.setOriginatingStaff(staffIdx);
        }
        pitchWheelEvents[channel].emplace(tick, event);
    };
    if (pianomaniaIt != _pianomaniaRanges.end()) {
        auto hasRangeStartingAt = [&](int tick) {
            return std::any_of(pianomaniaIt->second.cbegin(), pianomaniaIt->second.cend(), [tick](const auto& range) {
                return range.startTick == tick;
            });
        };
        for (const PianomaniaPitchWheelRange& range : pianomaniaIt->second) {
            auto terminalFunction = std::find_if(
                functions.functions.cbegin(), functions.functions.cend(), [&](const auto& function) {
                    return function.mStartTick == range.startTick && function.mEndTick == range.endTick;
                });
            if (terminalFunction != functions.functions.cend()) {
                int terminalValue = std::clamp(
                    _wheelSpec.mLimit + terminalFunction->func(range.endTick),
                    0,
                    2 * _wheelSpec.mLimit - 1);
                NPlayEvent terminal(ME_PITCHBEND, channel, terminalValue % 128, terminalValue / 128);
                terminal.setEffect(effect);
                if (staffInfoValid) {
                    terminal.setOriginatingStaff(staffIdx);
                }
                pitchWheelEvents[channel].emplace(range.endTick, terminal);
            }
            NPlayEvent center(ME_PITCHBEND, channel, _wheelSpec.mLimit % 128, _wheelSpec.mLimit / 128);
            if (staffInfoValid) {
                center.setOriginatingStaff(staffIdx);
            }
            pitchWheelEvents[channel].emplace(range.endTick, center);
            if (hasRangeStartingAt(range.endTick)) {
                continue;
            }
            addController(range.endTick, CTRL_HRPN, 0);
            addController(range.endTick, CTRL_LRPN, 0);
            addController(range.endTick, CTRL_HDATA, _wheelSpec.mAmplitude);
            addController(range.endTick, CTRL_LDATA, 0);
            addController(range.endTick, CTRL_HRPN, 127);
            addController(range.endTick, CTRL_LRPN, 127);
        }
        for (const PianomaniaPitchWheelRange& range : pianomaniaIt->second) {
            addController(range.startTick, CTRL_HRPN, 0);
            addController(range.startTick, CTRL_LRPN, 0);
            addController(range.startTick, CTRL_HDATA, range.sensitivitySemitones);
            addController(range.startTick, CTRL_LDATA, 0);
            addController(range.startTick, CTRL_HRPN, 127);
            addController(range.startTick, CTRL_LRPN, 127);
        }
    }

    std::map<int, int, std::greater<> > ranges;
    generateRanges(functions.functions, ranges);

    for (auto rit = ranges.crbegin(); rit != ranges.crend(); ++rit) {
        int32_t start = rit->first;
        int32_t end = rit->second;
        int32_t tick = start;

        std::vector<PitchWheelFunction> functionsToProcess;
        functionsToProcess.reserve(functions.functions.size());
        for (const auto& func : functions.functions) {
            if (func.mEndTick <= end) {
                functionsToProcess.push_back(func);
            }
        }
        std::vector<int> pitches;
        pitches.reserve(functionsToProcess.size());
        for (size_t i = 0; i < functionsToProcess.size(); ++i) {
            pitches.push_back(0);
        }
        int prevPitch = _wheelSpec.mLimit;
        bool forceUpdate = false;
        while (tick < end) {
            auto funcIt = functionsToProcess.begin();
            for (size_t i = 0; i < functionsToProcess.size(); ++i) {
                if (tick >= funcIt->mEndTick) {
                    // Function exceeds its max range
                    // don't need its value anymore
                    pitches.at(i) = 0;
                    ++funcIt;
                    forceUpdate = true;
                    continue;
                }
                if (tick < funcIt->mStartTick) {
                    ++funcIt;
                    continue;
                }
                int pitch = funcIt->func(tick);
                pitches.at(i) = pitch;
                ++funcIt;
            }
            int finalPitch = _wheelSpec.mLimit;
            for (const auto& pitch : pitches) {
                finalPitch += pitch;
            }
            finalPitch = std::clamp(finalPitch, 0, 2 * _wheelSpec.mLimit - 1);
            if (forceUpdate || finalPitch != prevPitch || tick == start) {
                NPlayEvent evb(ME_PITCHBEND, channel, finalPitch % 128, finalPitch / 128);
                evb.setEffect(effect);
                if (staffInfoValid) {
                    evb.setOriginatingStaff(staffIdx);
                }
                pitchWheelEvents[channel].emplace_hint(pitchWheelEvents[channel].end(), std::make_pair(tick, evb));
                forceUpdate = false;
            }

            prevPitch = finalPitch;
            tick += _wheelSpec.mStep;
        }
    }
}

int32_t PitchWheelRenderer::findNextStartTick(const std::vector<PitchWheelFunction>& functions) const noexcept
{
    int32_t tick = std::numeric_limits<int32_t>::max();
    for (const auto& func : functions) {
        tick = std::min(tick, func.mStartTick);
    }

    return tick;
}

int32_t PitchWheelRenderer::calculatePitchBend(const std::vector<PitchWheelFunction>& functions, int32_t tick) const noexcept
{
    int pitchValue = _wheelSpec.mLimit;

    for (const auto& func : functions) {
        if (tick < func.mStartTick || tick >= func.mEndTick) {
            continue;
        }

        pitchValue += func.func(tick);
    }

    return pitchValue;
}

// 1                |------|
// 2                   |-----------| handleStartTick();
// result           |--------------|
// 3           |--------|            handleEndTick();
// result      |-------------------|
void PitchWheelRenderer::generateRanges(const std::vector<PitchWheelFunction>& functions, std::map<int, int, std::greater<> >& ranges)
{
    // !NOTE ranges map is reversed. Use reverse iterators
    auto handleEndTick = [&](const PitchWheelFunction& func) {
        auto lowerRange = ranges.upper_bound(func.mEndTick);
        if (lowerRange == ranges.end()) {
            return false;
        }
        ranges.insert({ func.mStartTick, lowerRange->second });
        ranges.erase(lowerRange);
        return true;
    };

    auto handleStartTick = [&](const PitchWheelFunction& func) {
        auto lowerRange = ranges.upper_bound(func.mStartTick);
        if (lowerRange == ranges.end()) {
            // We are getting PW events sorted by startTick
            // So in ideal world handleEndTick always return false
            return handleEndTick(func);
        }
        if (lowerRange->second >= func.mStartTick) {
            lowerRange->second = std::max(lowerRange->second, func.mEndTick);
            return true;
        }
        return false;
    };

    for (const auto& func : functions) {
        if (auto key = ranges.find(func.mStartTick); key != ranges.end()) {
            key->second = std::max(key->second, func.mEndTick);
            continue;
        }
        if (!handleStartTick(func)) {
            ranges.insert({ func.mStartTick, func.mEndTick });
        }
    }
}
