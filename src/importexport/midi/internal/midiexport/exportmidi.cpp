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

#include "exportmidi.h"

#include "engraving/dom/chordrest.h"
#include "engraving/dom/key.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/repeatlist.h"
#include "engraving/dom/sig.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/synthesizerstate.h"
#include "engraving/dom/tempo.h"

#include "engraving/compat/midi/event.h"
#include "engraving/compat/midi/compatmidirender.h"

#include "log.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

using namespace mu::engraving;

namespace mu::iex::midi {
static constexpr int DEFAULT_NOTE_OFF_VELOCITY = 64;
static constexpr int ORNAMENT_NOTE_OFF_VELOCITY = 127;

static bool isNoteOn(const MidiEvent& event)
{
    return event.type() == ME_NOTEON && event.velo() > 0;
}

static bool isNoteOff(const MidiEvent& event)
{
    return event.type() == ME_NOTEOFF || (event.type() == ME_NOTEON && event.velo() == 0);
}

static int midiEventPriority(const MidiEvent& event)
{
    if (isNoteOff(event)) {
        return 0;
    }
    if (isNoteOn(event)) {
        return 2;
    }
    return 1;
}

static MidiEvent makeNoteOff(int channel, int pitch, int velocity)
{
    MidiEvent event;
    event.setType(ME_NOTEOFF);
    event.setChannel(channel);
    event.setPitch(pitch);
    event.setVelo(velocity);
    return event;
}

static MidiEvent makeNoteOn(int channel, int pitch, int velocity, const Note* note)
{
    MidiEvent event(ME_NOTEON, channel, pitch, velocity);
    event.setPianomaniaSourceNote(note);
    return event;
}

static MidiEvent makeSourceNoteOff(int channel, int pitch, int velocity, const Note* note)
{
    MidiEvent event = makeNoteOff(channel, pitch, velocity);
    event.setPianomaniaSourceNote(note);
    return event;
}

static int mergeSameStartNoteOffVelocity(int existingVelocity, int incomingVelocity, bool incomingExtendsEnd)
{
    const bool existingIsOrnament = existingVelocity == ORNAMENT_NOTE_OFF_VELOCITY;
    const bool incomingIsOrnament = incomingVelocity == ORNAMENT_NOTE_OFF_VELOCITY;
    if (existingIsOrnament != incomingIsOrnament) {
        return existingIsOrnament ? incomingVelocity : existingVelocity;
    }

    return incomingExtendsEnd ? incomingVelocity : existingVelocity;
}

static void sortMidiEvents(std::vector<std::tuple<int, MidiEvent, size_t> >& events)
{
    std::stable_sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
        if (std::get<0>(left) != std::get<0>(right)) {
            return std::get<0>(left) < std::get<0>(right);
        }

        int leftPriority = midiEventPriority(std::get<1>(left));
        int rightPriority = midiEventPriority(std::get<1>(right));
        if (leftPriority != rightPriority) {
            return leftPriority < rightPriority;
        }

        return std::get<2>(left) < std::get<2>(right);
    });
}

static void canonicalizeMergedOrigins(std::vector<MidiNoteOrigin>& origins)
{
    const bool hasVisualOrigin = std::any_of(origins.cbegin(), origins.cend(), [](const MidiNoteOrigin& origin) {
        return !origin.nonVisualOrnament;
    });

    std::set<std::tuple<const Note*, bool, int, int> > seen;
    origins.erase(std::remove_if(origins.begin(), origins.end(), [&](const MidiNoteOrigin& origin) {
        // One MIDI attack can be shared by an explicitly written note and an
        // ornament renderer attack at the same pitch and tick. The written
        // note owns that audible attack; the coincident ornament is not a
        // second audible event and therefore must not create an ornament
        // occurrence. Identical renderer origins are likewise one origin.
        if (hasVisualOrigin && origin.nonVisualOrnament) {
            return true;
        }
        return !seen.emplace(origin.note, origin.nonVisualOrnament,
                             origin.sourceStartTick, origin.sourceEndTick).second;
    }), origins.end());
}

static void normalizeSamePitchNotes(MidiTrack& track)
{
    using NoteKey = std::pair<int, int>;

    struct OpenNote {
        int start = 0;
        MidiEvent onEvent;
        size_t sequence = 0;
    };

    struct NoteInterval {
        int start = 0;
        int end = 0;
        MidiEvent onEvent;
        int offVelocity = DEFAULT_NOTE_OFF_VELOCITY;
        size_t sequence = 0;
        std::vector<MidiNoteOrigin> origins;
    };

    auto& trackEvents = track.events();
    std::vector<std::tuple<int, MidiEvent, size_t> > ordered;
    ordered.reserve(trackEvents.size());

    size_t sequence = 0;
    for (const auto& item : trackEvents) {
        ordered.emplace_back(item.first, item.second, sequence++);
    }
    sortMidiEvents(ordered);

    std::map<NoteKey, std::vector<OpenNote> > openNotes;
    std::map<NoteKey, std::vector<NoteInterval> > intervalsByPitch;
    std::vector<std::tuple<int, MidiEvent, size_t> > rebuilt;
    rebuilt.reserve(trackEvents.size());

    for (const auto& item : ordered) {
        int tick = std::get<0>(item);
        const MidiEvent& event = std::get<1>(item);
        size_t eventSequence = std::get<2>(item);
        NoteKey key(event.channel(), event.pitch());

        if (isNoteOn(event)) {
            openNotes[key].push_back({ tick, event, eventSequence });
        } else if (isNoteOff(event)) {
            auto openIt = openNotes.find(key);
            if (openIt == openNotes.end() || openIt->second.empty()) {
                continue;
            }

            auto& candidates = openIt->second;
            auto openPosition = candidates.end();
            if (const Note* offSourceNote = event.pianomaniaSourceNote()) {
                auto reversePosition = std::find_if(candidates.rbegin(), candidates.rend(),
                                                    [offSourceNote](const OpenNote& candidate) {
                    return candidate.onEvent.pianomaniaSourceNote() == offSourceNote;
                });
                if (reversePosition != candidates.rend()) {
                    openPosition = std::prev(reversePosition.base());
                }
            }
            if (openPosition == candidates.end()) {
                openPosition = std::prev(candidates.end());
            }
            OpenNote open = *openPosition;
            candidates.erase(openPosition);
            if (openIt->second.empty()) {
                openNotes.erase(openIt);
            }

            const Note* sourceNote = open.onEvent.pianomaniaSourceNote();
            if (!sourceNote) {
                sourceNote = event.pianomaniaSourceNote();
            }
            std::vector<MidiNoteOrigin> origins;
            if (sourceNote) {
                origins.push_back({ sourceNote, event.velo() == ORNAMENT_NOTE_OFF_VELOCITY,
                                    open.start, tick, open.sequence });
            }
            intervalsByPitch[key].push_back({ open.start, tick, open.onEvent, event.velo(), open.sequence, origins });
        } else {
            rebuilt.push_back(item);
        }
    }

    for (const auto& [key, notes] : openNotes) {
        for (const OpenNote& open : notes) {
            std::vector<MidiNoteOrigin> origins;
            if (const Note* sourceNote = open.onEvent.pianomaniaSourceNote()) {
                origins.push_back({ sourceNote, false, open.start, open.start, open.sequence });
            }
            intervalsByPitch[key].push_back({ open.start, open.start, open.onEvent, DEFAULT_NOTE_OFF_VELOCITY,
                                              open.sequence, origins });
        }
    }

    for (auto& [key, intervals] : intervalsByPitch) {
        std::stable_sort(intervals.begin(), intervals.end(), [](const NoteInterval& left, const NoteInterval& right) {
            if (left.start != right.start) {
                return left.start < right.start;
            }
            return left.sequence < right.sequence;
        });

        std::vector<NoteInterval> merged;
        merged.reserve(intervals.size());
        for (const NoteInterval& interval : intervals) {
            if (!merged.empty() && merged.back().start == interval.start) {
                const bool incomingExtendsEnd = interval.end > merged.back().end;
                merged.back().offVelocity = mergeSameStartNoteOffVelocity(
                    merged.back().offVelocity,
                    interval.offVelocity,
                    incomingExtendsEnd
                );
                if (incomingExtendsEnd) {
                    merged.back().end = interval.end;
                }
                merged.back().origins.insert(merged.back().origins.end(), interval.origins.begin(), interval.origins.end());
                canonicalizeMergedOrigins(merged.back().origins);
                continue;
            }
            merged.push_back(interval);
        }

        for (size_t i = 1; i < merged.size(); ++i) {
            if (merged[i - 1].end > merged[i].start) {
                merged[i - 1].end = merged[i].start;
            }
        }

        for (const NoteInterval& interval : merged) {
            MidiEvent onEvent = interval.onEvent;
            onEvent.setPianomaniaNoteOrigins(interval.origins);
            MidiEvent offEvent = makeNoteOff(interval.onEvent.channel(), interval.onEvent.pitch(), interval.offVelocity);
            offEvent.setPianomaniaNoteOrigins(interval.origins);
            rebuilt.emplace_back(interval.start, onEvent, interval.sequence);
            rebuilt.emplace_back(interval.end, offEvent, interval.sequence);
        }
    }

    sortMidiEvents(rebuilt);

    std::multimap<int, MidiEvent> normalized;
    for (const auto& item : rebuilt) {
        normalized.insert({ std::get<0>(item), std::get<1>(item) });
    }

    trackEvents = std::move(normalized);
}

//---------------------------------------------------------
//   writeHeader
//---------------------------------------------------------

void ExportMidi::writeHeader(const CompatMidiRendererInternal::Context& context)
{
    if (m_midiFile.tracks().empty()) {
        return;
    }
    MidiTrack& track  = m_midiFile.tracks().front();

    //--------------------------------------------
    //    write track names
    //--------------------------------------------

    int staffIdx = 0;
    for (auto& track1: m_midiFile.tracks()) {
        Staff* staff  = m_score->staff(staffIdx);

        muse::ByteArray partName = staff->partName().toUtf8();
        size_t len = partName.size() + 1;
        std::vector<unsigned char> data(partName.constData(), partName.constData() + len);

        MidiEvent ev;
        ev.setType(ME_META);
        ev.setMetaType(META_TRACK_NAME);
        ev.setEData(std::move(data));
        ev.setLen(static_cast<int>(len));

        track1.insert(0, ev);

        ++staffIdx;
    }

    //--------------------------------------------
    //    write time signature
    //--------------------------------------------

    TimeSigMap* sigmap = m_score->sigmap();
    for (const RepeatSegment* rs : m_score->repeatList()) {
        int startTick  = rs->tick;
        int endTick    = rs->endTick();
        int tickOffset = rs->utick - rs->tick;

        auto bs = sigmap->lower_bound(startTick);
        auto es = sigmap->lower_bound(endTick);

        for (auto is = bs; is != es; ++is) {
            SigEvent se = is->second;
            Fraction ts(se.timesig());
            int n;
            switch (ts.denominator()) {
            case 1:  n = 0;
                break;
            case 2:  n = 1;
                break;
            case 4:  n = 2;
                break;
            case 8:  n = 3;
                break;
            case 16: n = 4;
                break;
            case 32: n = 5;
                break;
            default:
                n = 2;
                LOGD("ExportMidi: unknown time signature %s",
                     qPrintable(ts.toString()));
                break;
            }

            MidiEvent ev;
            ev.setType(ME_META);
            ev.setMetaType(META_TIME_SIGNATURE);
            ev.setLen(4);
            ev.setEData({ static_cast<unsigned char>(ts.numerator()),
                          static_cast<unsigned char>(n),
                          24,
                          8 });
            track.insert(CompatMidiRender::tick(context, is->first + tickOffset), ev);
        }
    }

    //---------------------------------------------------
    //    write key signatures
    //    assume every staff corresponds to a midi track
    //---------------------------------------------------

    staffIdx = 0;
    for (auto& track1: m_midiFile.tracks()) {
        Staff* staff  = m_score->staff(staffIdx);
        KeyList* keys = staff->keyList();

        bool initialKeySigFound = false;
        for (const RepeatSegment* rs : m_score->repeatList()) {
            int startTick  = rs->tick;
            int endTick    = startTick + rs->len();
            int tickOffset = rs->utick - rs->tick;

            auto sk = keys->lower_bound(startTick);
            auto ek = keys->lower_bound(endTick);

            for (auto ik = sk; ik != ek; ++ik) {
                MidiEvent ev;
                ev.setType(ME_META);
                Key key = ik->second.concertKey();           // -7 -- +7
                ev.setMetaType(META_KEY_SIGNATURE);
                ev.setLen(2);
                ev.setEData({ static_cast<unsigned char>(key), 0 /* major */ });
                int tick = ik->first + tickOffset;
                track1.insert(CompatMidiRender::tick(context, tick), ev);
                if (tick == 0) {
                    initialKeySigFound = true;
                }
            }
        }

        // fall back write a default C keysig if no initial keysig found
        if (!initialKeySigFound) {
            MidiEvent ev;
            ev.setType(ME_META);
            ev.setMetaType(META_KEY_SIGNATURE);
            ev.setLen(2);
            ev.setEData({ 0 /* key */, 0 /* major */ });
            track1.insert(0, ev);
        }

        ++staffIdx;
    }

    //--------------------------------------------
    //    write tempo changes from PauseMap
    //     don't need to unwind or add pauses as this was done already
    //--------------------------------------------

    if (!context.applyCaesuras) {
        return;
    }

    const TempoMap* tempomap = context.pauseMap->tempomapWithPauses();
    BeatsPerSecond tempoMultiplier = tempomap->tempoMultiplier();
    for (auto it = tempomap->cbegin(); it != tempomap->cend(); ++it) {
        MidiEvent ev;
        ev.setType(ME_META);
        //
        // compute midi tempo: microseconds / quarter note
        //
        int tempo = lrint((1.0 / it->second.tempo.val * tempoMultiplier.val) * 1000000.0);

        ev.setMetaType(META_TEMPO);
        ev.setLen(3);
        ev.setEData({ static_cast<unsigned char>(tempo >> 16),
                      static_cast<unsigned char>(tempo >> 8),
                      static_cast<unsigned char>(tempo) });
        track.insert(it->first, ev);
    }
}

bool ExportMidi::collectPianomaniaProvenance(ExportedMidiProvenance* provenance) const
{
    if (!provenance) {
        return true;
    }

    struct OpenNote {
        ExportedMidiEventLocator locator;
        int velocity = 0;
        std::vector<MidiNoteOrigin> origins;
    };

    provenance->division = m_midiFile.division();
    provenance->audibleEvents.clear();

    for (size_t trackIndex = 0; trackIndex < m_midiFile.tracks().size(); ++trackIndex) {
        const MidiTrack& track = m_midiFile.tracks()[trackIndex];
        std::map<std::pair<int, int>, std::vector<OpenNote> > openNotes;
        int eventOrdinal = 0;
        for (const auto& item : track.events()) {
            const int tick = item.first;
            const MidiEvent& event = item.second;
            if (!isNoteOn(event) && !isNoteOff(event)) {
                ++eventOrdinal;
                continue;
            }

            ExportedMidiEventLocator locator;
            locator.track = static_cast<int>(trackIndex);
            locator.eventOrdinal = eventOrdinal;
            locator.absoluteTick = tick;
            locator.status = event.type() | event.channel();
            locator.channel = event.channel();
            locator.data = { event.pitch(), event.velo() };

            const std::pair<int, int> key(event.channel(), event.pitch());
            if (isNoteOn(event)) {
                openNotes[key].push_back({ locator, event.velo(), event.pianomaniaNoteOrigins() });
                ++eventOrdinal;
                continue;
            }

            auto openIt = openNotes.find(key);
            if (openIt == openNotes.end() || openIt->second.empty()) {
                LOGE() << "Cannot build Pianomania MIDI provenance: unmatched note-off at track "
                       << trackIndex << ", event " << eventOrdinal;
                return false;
            }

            OpenNote open = openIt->second.back();
            openIt->second.pop_back();
            if (openIt->second.empty()) {
                openNotes.erase(openIt);
            }

            std::vector<MidiNoteOrigin> origins = event.pianomaniaNoteOrigins();
            if (origins.empty()) {
                origins = std::move(open.origins);
            }
            if (origins.empty()) {
                LOGE() << "Cannot build Pianomania MIDI provenance: audible event has no score origin at track "
                       << trackIndex << ", event " << eventOrdinal;
                return false;
            }
            const bool hasVisualOrigin = std::any_of(origins.cbegin(), origins.cend(), [](const MidiNoteOrigin& origin) {
                return !origin.nonVisualOrnament;
            });
            const bool hasOrnamentOrigin = std::any_of(origins.cbegin(), origins.cend(), [](const MidiNoteOrigin& origin) {
                return origin.nonVisualOrnament;
            });
            if (hasVisualOrigin && hasOrnamentOrigin) {
                LOGE() << "Cannot build Pianomania MIDI provenance: visual and ornament origins share one audible event at track "
                       << trackIndex << ", event " << eventOrdinal;
                return false;
            }
            if (hasOrnamentOrigin != (event.velo() == ORNAMENT_NOTE_OFF_VELOCITY)) {
                LOGE() << "Cannot build Pianomania MIDI provenance: ornament origin disagrees with note-off velocity at track "
                       << trackIndex << ", event " << eventOrdinal;
                return false;
            }
            provenance->audibleEvents.push_back({
                event.pitch(),
                event.channel(),
                open.locator.absoluteTick,
                tick,
                open.velocity,
                event.velo(),
                open.locator,
                locator,
                std::move(origins),
            });
            ++eventOrdinal;
        }

        if (!openNotes.empty()) {
            LOGE() << "Cannot build Pianomania MIDI provenance: unclosed note-on in track " << trackIndex;
            return false;
        }
    }

    std::sort(provenance->audibleEvents.begin(), provenance->audibleEvents.end(),
              [](const ExportedMidiAudibleEvent& left, const ExportedMidiAudibleEvent& right) {
        return std::tie(left.noteOn.track, left.noteOn.eventOrdinal)
               < std::tie(right.noteOn.track, right.noteOn.eventOrdinal);
    });
    return true;
}

//---------------------------------------------------------
//  write
//    export midi file
//    return false on error
//
//    The 3rd and 4th versions of write create a temporary, uninitialized synth state
//    so we can render the midi - it should fall back correctly to the defaults, with a warning.
//    These should only be used for tests. When actually rendering midi as a user action,
//    make sure to use the 1st and 2nd versions, passing the global musescore synth state
//    from mscore->synthesizerState() as the synthState parameter.
//---------------------------------------------------------

bool ExportMidi::write(QIODevice* device, bool midiExpandRepeats, bool exportRPNs, const SynthesizerState& synthState)
{
    return write(device, midiExpandRepeats, exportRPNs, synthState, nullptr);
}

bool ExportMidi::write(QIODevice* device, bool midiExpandRepeats, bool exportRPNs, const SynthesizerState& synthState,
                       ExportedMidiProvenance* provenance)
{
    m_midiFile.setDivision(Constants::DIVISION);
    m_midiFile.setFormat(1);
    std::vector<MidiTrack>& tracks = m_midiFile.tracks();

    for (size_t i = 0; i < m_score->nstaves(); ++i) {
        tracks.push_back(MidiTrack());
    }

    EventsHolder events;
    CompatMidiRendererInternal::Context context;
    context.eachStringHasChannel = false;
    context.instrumentsHaveEffects = false;
    context.harmonyChannelSetting = CompatMidiRendererInternal::HarmonyChannelSetting::DEFAULT;
    context.sndController = CompatMidiRender::getControllerForSnd(m_score, synthState.ccToUse());
    context.useDefaultArticulations = false;
    context.applyCaesuras = true;

    CompatMidiRender::renderScore(m_score, events, context, midiExpandRepeats);

    staff_idx_t staffIdx = 0;
    for (auto& track: tracks) {
        Staff* staff = m_score->staff(staffIdx);
        Part* part   = staff->part();

        track.setOutPort(part->midiPort());
        track.setOutChannel(part->midiChannel());

        // Pass through the all instruments in the part
        for (const auto& pair : part->instruments()) {
            // Pass through the all channels of the instrument
            // "normal", "pizzicato", "tremolo" for Strings,
            // "normal", "mute" for Trumpet
            for (const InstrChannel* instrChan : pair.second->channel()) {
                const InstrChannel* ch = part->masterScore()->playbackChannel(instrChan);
                char port    = part->masterScore()->midiPort(ch->channel());
                char channel = part->masterScore()->midiChannel(ch->channel());

                if (staff->isTop()) {
                    track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_RESET_ALL_CTRL, 0));
                    // We need this to get the correct pitch of bends
                    // Hidden under preferences because some software
                    // crashes when receiving RPNs: https://musescore.org/en/node/37431
                    if (channel != 9 && exportRPNs) {
                        // set pitch bend sensitivity to 12 semitones:
                        track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_LRPN, 0));
                        track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_HRPN, 0));
                        track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_HDATA, 12));

                        // reset fine tuning
                        /*track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_LRPN, 1));
                        track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_HRPN, 0));
                        track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_HDATA, 64));*/

                        // deactivate rpn
                        track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_LRPN, 127));
                        track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_HRPN, 127));
                    }

                    if (ch->program() != -1) {
                        track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_PROGRAM, ch->program()));
                    }
                    track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_VOLUME, ch->volume()));
                    track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_PANPOT, ch->pan()));
                    track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_REVERB_SEND, ch->reverb()));
                    track.insert(0, MidiEvent(ME_CONTROLLER, channel, CTRL_CHORUS_SEND, ch->chorus()));
                }

                // Export port to MIDI META event
                if (track.outPort() >= 0 && track.outPort() <= 127) {
                    MidiEvent ev;
                    ev.setType(ME_META);
                    ev.setMetaType(META_PORT_CHANGE);
                    ev.setLen(1);
                    ev.setEData({ static_cast<unsigned char>(track.outPort()) });
                    track.insert(0, ev);
                }

                for (size_t e = 0; e < events.size(); ++e) {
                    auto& multimap = events[e];
                    for (auto& item : multimap) {
                        const NPlayEvent& event = item.second;
                        if (event.isMuted()) {
                            continue;
                        }
                        staff_idx_t equivalentStaffIdx = staffIdx;
                        for (Staff* st : m_score->masterScore()->staves()) {
                            if (staff->id() == st->id()) {
                                equivalentStaffIdx = st->idx();
                            }
                        }

                        if (event.getOriginatingStaff() != equivalentStaffIdx) {
                            continue;
                        }

                        if (!exportRPNs && event.type() == ME_CONTROLLER && event.portamento()) {
                            // ignore portamento control events if exportRPN isn't switched on
                            continue;
                        }

                        char eventPort    = m_score->masterScore()->midiPort(event.channel());
                        char eventChannel = m_score->masterScore()->midiChannel(event.channel());
                        if (port != eventPort || channel != eventChannel) {
                            continue;
                        }

                        if (event.type() == ME_NOTEON) {
                            // use the note values instead of the event values if portamento is suppressed
                            if (!exportRPNs && event.portamento()) {
                                track.insert(CompatMidiRender::tick(context, item.first),
                                             makeNoteOn(channel, event.note()->pitch(), event.velo(), event.note()));
                            } else {
                                track.insert(CompatMidiRender::tick(context, item.first),
                                             makeNoteOn(channel, event.pitch(), event.velo(), event.note()));
                            }
                        } else if (event.type() == ME_NOTEOFF) {
                            track.insert(CompatMidiRender::tick(context, item.first),
                                         makeSourceNoteOff(channel, event.pitch(), event.velo(), event.note()));
                        } else if (event.type() == ME_CONTROLLER) {
                            track.insert(CompatMidiRender::tick(context, item.first), MidiEvent(ME_CONTROLLER, channel,
                                                                                                event.controller(),
                                                                                                event.value()));
                        } else if (event.type() == ME_PITCHBEND) {
                            track.insert(CompatMidiRender::tick(context, item.first), MidiEvent(ME_PITCHBEND, channel,
                                                                                                event.dataA(), event.dataB()));
                        } else {
                            LOGD("writeMidi: unknown midi event 0x%02x", event.type());
                        }
                    }
                }
            }
        }

        // Export lyrics and RehearsalMarks as Meta events
        for (const RepeatSegment* rs : m_score->repeatList()) {
            int endTick    = rs->endTick();
            int tickOffset = rs->utick - rs->tick;

            // export Lyrics
            SegmentType st = SegmentType::ChordRest;
            for (Segment* seg = rs->firstMeasure()->first(st); seg && seg->tick().ticks() < endTick; seg = seg->next1(st)) {
                for (track_idx_t i = part->startTrack(); i < part->endTrack(); ++i) {
                    ChordRest* cr = toChordRest(seg->element(i));
                    if (cr) {
                        for (const auto& lyric : cr->lyrics()) {
                            LyricsSyllabic syllabic = lyric->syllabic();
                            muse::ByteArray lyricText = lyric->plainText().toUtf8();
                            if ((syllabic == LyricsSyllabic::SINGLE || syllabic == LyricsSyllabic::END)
                                && (lyricText.empty() || lyricText[lyricText.size() - 1] != ' ')) {
                                lyricText.push_back(' ');
                            }

                            size_t len = lyricText.size() + 1;
                            std::vector<unsigned char> data(lyricText.constData(), lyricText.constData() + len);

                            MidiEvent ev;
                            ev.setType(ME_META);
                            ev.setMetaType(META_LYRIC);
                            ev.setEData(std::move(data));
                            ev.setLen(static_cast<int>(len));

                            int tick = cr->tick().ticks() + tickOffset;
                            track.insert(CompatMidiRender::tick(context, tick), ev);
                        }
                    }
                }
            }

            // export RehearsalMarks only for first track
            if (staffIdx == 0) {
                for (Segment* seg = rs->firstMeasure()->first(Segment::CHORD_REST_OR_TIME_TICK_TYPE);
                     seg && seg->tick().ticks() < endTick;
                     seg = seg->next1(Segment::CHORD_REST_OR_TIME_TICK_TYPE)) {
                    for (EngravingItem* e : seg->annotations()) {
                        if (e->isRehearsalMark()) {
                            RehearsalMark* r = toRehearsalMark(e);
                            muse::ByteArray rText = r->plainText().toUtf8();
                            size_t len = rText.size() + 1;
                            std::vector<unsigned char> data(rText.constData(), rText.constData() + len);

                            MidiEvent ev;
                            ev.setType(ME_META);
                            ev.setMetaType(META_MARKER);
                            ev.setEData(std::move(data));
                            ev.setLen(static_cast<int>(len));

                            int tick = r->segment()->tick().ticks() + tickOffset;
                            track.insert(CompatMidiRender::tick(context, tick), ev);
                        }
                    }
                }
            }
        }
        ++staffIdx;
    }
    if (tracks.size() > 1) {
        MidiTrack& mainTrack = tracks.front();
        auto isDuplicate = [](const MidiTrack& t, int tick, const MidiEvent& ev) {
            if (isNoteOn(ev) || isNoteOff(ev)) {
                return false;
            }

            auto range = t.events().equal_range(tick);
            for (auto it = range.first; it != range.second; ++it) {
                const MidiEvent& existing = it->second;
                if (existing.type() == ev.type() && existing.channel() == ev.channel()
                    && existing.dataA() == ev.dataA() && existing.dataB() == ev.dataB()
                    && existing.metaType() == ev.metaType() && existing.len() == ev.len()
                    && (ev.len() == 0 || std::memcmp(existing.edata(), ev.edata(), ev.len()) == 0)) {
                    return true;
                }
            }
            return false;
        };

        for (size_t i = 1; i < tracks.size(); ++i) {
            for (const auto& item : tracks[i].events()) {
                if (!isDuplicate(mainTrack, item.first, item.second)) {
                    mainTrack.insert(item.first, item.second);
                }
            }
        }
        tracks.erase(tracks.begin() + 1, tracks.end());
    }
    if (!tracks.empty()) {
        normalizeSamePitchNotes(tracks.front());
    }

    m_midiFile.setFormat(0);
    writeHeader(context);
    if (!collectPianomaniaProvenance(provenance)) {
        return false;
    }
    return !m_midiFile.write(device);
}

bool ExportMidi::write(const QString& name, bool midiExpandRepeats, bool exportRPNs, const SynthesizerState& synthState)
{
    return write(name, midiExpandRepeats, exportRPNs, synthState, nullptr);
}

bool ExportMidi::write(const QString& name, bool midiExpandRepeats, bool exportRPNs, const SynthesizerState& synthState,
                       ExportedMidiProvenance* provenance)
{
    m_file.setFileName(name);
    if (!m_file.open(QIODevice::WriteOnly)) {
        return false;
    }
    return write(&m_file, midiExpandRepeats, exportRPNs, synthState, provenance);
}

bool ExportMidi::write(QIODevice* device, bool midiExpandRepeats, bool exportRPNs)
{
    SynthesizerState ss;
    return write(device, midiExpandRepeats, exportRPNs, ss);
}

bool ExportMidi::write(const QString& name, bool midiExpandRepeats, bool exportRPNs)
{
    SynthesizerState ss;
    return write(name, midiExpandRepeats, exportRPNs, ss);
}
}
