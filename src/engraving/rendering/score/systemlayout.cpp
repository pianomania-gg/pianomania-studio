/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2023 MuseScore Limited and others
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
#include <algorithm>
#include <array>
#include <cfloat>
#include <map>
#include <set>
#include <vector>

#include "systemlayout.h"

#include "realfn.h"

#include "style/defaultstyle.h"

#include "dom/articulation.h"
#include "dom/barline.h"
#include "dom/beam.h"
#include "dom/box.h"
#include "dom/bracket.h"
#include "dom/bracketItem.h"
#include "dom/chord.h"
#include "dom/dynamic.h"
#include "dom/fermata.h"
#include "dom/factory.h"
#include "dom/fingering.h"
#include "dom/guitarbend.h"
#include "dom/hairpin.h"
#include "dom/hook.h"
#include "dom/instrumentname.h"
#include "dom/layoutbreak.h"
#include "dom/lyrics.h"
#include "dom/measure.h"
#include "dom/measurenumber.h"
#include "dom/mmrest.h"
#include "dom/mmrestrange.h"
#include "dom/mscore.h"
#include "dom/note.h"
#include "dom/ornament.h"
#include "dom/part.h"
#include "dom/parenthesis.h"
#include "dom/pedal.h"
#include "dom/playcounttext.h"
#include "dom/rest.h"
#include "dom/score.h"
#include "dom/slur.h"
#include "dom/spacer.h"
#include "dom/staff.h"
#include "dom/stafflines.h"
#include "dom/stem.h"
#include "dom/system.h"
#include "dom/tie.h"
#include "dom/timesig.h"
#include "dom/tremolobar.h"
#include "dom/tremolosinglechord.h"
#include "dom/tremolotwochord.h"
#include "dom/tuplet.h"
#include "dom/volta.h"
#include "dom/whammybar.h"

#include "tlayout.h"
#include "alignmentlayout.h"
#include "autoplace.h"
#include "beamlayout.h"
#include "beamtremololayout.h"
#include "boxlayout.h"
#include "chordlayout.h"
#include "guitarbendlayout.h"
#include "guitardivelayout.h"
#include "harmonylayout.h"
#include "lyricslayout.h"
#include "measurelayout.h"
#include "tupletlayout.h"
#include "restlayout.h"
#include "slurtielayout.h"
#include "horizontalspacing.h"
#include "dynamicslayout.h"

#include "defer.h"
#include "log.h"

using namespace mu::engraving;
using namespace mu::engraving::rendering::score;

namespace {
// Pianomania fingering placement clearances, in spatium units. The "preferred"
// values match the vanilla fingering minDistance so undisturbed fingerings stay
// at their vanilla position; the "min" values are the squeeze floor used when
// tucking a fingering into the pocket between the notes and a slur.
constexpr double PM_FINGERING_NOTE_CLEARANCE = 0.5;
constexpr double PM_FINGERING_NOTE_CLEARANCE_MIN = 0.15;
constexpr double PM_FINGERING_STAFF_CLEARANCE = 0.5;
constexpr double PM_FINGERING_STAFF_CLEARANCE_MIN = 0.10;
constexpr double PM_FINGERING_MARK_CLEARANCE = 0.25;
constexpr double PM_FINGERING_MARK_CLEARANCE_MIN = 0.15;
// Tucks may nearly touch articulation marks (staccato dots, tenuto bars):
// a digit centered tightly over dot and notehead beats one dodged sideways.
constexpr double PM_FINGERING_MARK_CLEARANCE_TUCK = 0.08;
// Slur clearance is measured from the curve centerline; the segment's
// midThickness is added on top. Kept intentionally tight so fingerings hug
// slurs instead of floating away from them. The tuck variant is the squeeze
// floor for fitting a digit into the pocket between the notes and a slur:
// a digit may sit nearly touching the curve there.
constexpr double PM_FINGERING_SLUR_CLEARANCE = 0.25;
constexpr double PM_FINGERING_SLUR_CLEARANCE_TUCK = 0.06;
// Move targets overshoot the conflict-test clearance by this much so a second
// pass (page-stage rerun, persistent replay) sees the result as already clear.
constexpr double PM_FINGERING_SLUR_TARGET_EXTRA = 0.05;
constexpr double PM_FINGERING_SLUR_X_MARGIN = 0.15;
// A tuck may descend at most this far below the vanilla autoplace position:
// vanilla cleared unmodeled obstacles (stems, beams, accidentals) with skyline
// padding, so a small descent cannot land on them. Deeper reaches into the
// pocket must be proven clear against the staff skyline instead.
constexpr double PM_FINGERING_TUCK_MAX_BACKOFF = 0.25;
// A tucked digit may dip this far into the staff (its bottom past the top
// line for above-placement) when the pocket under a slur is otherwise clear:
// squeezing over the outermost staff line beats floating above the slur.
constexpr double PM_FINGERING_TUCK_STAFF_INTRUSION = 0.5;
// Displacement (sp) beyond which a below-preferred group is evaluated on the
// opposite side, and the advantage (sp) the opposite side must win by.
constexpr double PM_FINGERING_FLIP_TRIGGER = 2.25;
constexpr double PM_FINGERING_FLIP_ADVANTAGE = 1.0;
constexpr double PM_FINGERING_DESPERATE_MOVE = 1.4;
constexpr double PM_FINGERING_NOTEHEAD_DETACHMENT_CAP = 3.0;
constexpr double PM_TEXT_HAIRPIN_NOTATION_CLEARANCE = 0.25;
constexpr double PM_TEXT_HAIRPIN_MIN_OVERLAP = 0.05;

struct GroupPlacement {
    double dx = 0.0;
    double moveAway = 0.0; // in the away-from-staff direction; negative = tucked toward the notes
    bool slurResolved = true;
    bool tucked = false;
    // How far past the vanilla position this placement actually tucked, and
    // how much local clearance proof is available to neighboring groups in
    // the same slur run.
    double tuckAllowanceUsed = 0.0;
    double tuckAllowanceProven = 0.0;
};

enum class FingeringObstacleKind {
    Slur,
    Mark,
    Tuplet,
    Rest,
    DynamicZone
};

struct FingeringObstacle {
    FingeringObstacleKind kind = FingeringObstacleKind::Mark;
    const SlurSegment* slurSegment = nullptr;
    // Reserved landing zone of a note-attached (sf-family) dynamic: the
    // dynamic owns the tight spot against its chord; same-segment fingering
    // groups dodge left of it instead of displacing it. Not a rect obstacle.
    const Segment* dynamicSegment = nullptr;
    RectF rect;
    staff_idx_t staffIdx = muse::nidx;
    bool above = true;
};

struct TextHairpinObstacle {
    RectF rect;
};

RectF tupletMarkerSystemRect(const Tuplet* tuplet);

// Dynamics that modify a specific note or chord (sforzando family), as
// opposed to passage dynamics (p, f, mf, ...) that mark a span of music.
bool isPianomaniaNoteModifierDynamic(DynamicType type)
{
    switch (type) {
    case DynamicType::SF:
    case DynamicType::SFZ:
    case DynamicType::SFF:
    case DynamicType::SFFZ:
    case DynamicType::SFFF:
    case DynamicType::SFFFZ:
    case DynamicType::SFP:
    case DynamicType::SFPP:
    case DynamicType::RFZ:
    case DynamicType::RF:
    case DynamicType::FZ:
        return true;
    default:
        return false;
    }
}

bool hasManualFingeringPlacement(const Fingering* fingering)
{
    return !fingering->autoplace()
           || !fingering->isStyled(Pid::OFFSET)
           || !fingering->offset().isNull()
           || !fingering->isStyled(Pid::MIN_DISTANCE)
           || fingering->propertyFlags(Pid::PLACEMENT) != PropertyFlags::STYLED;
}

double staffYInSystem(const System* system, staff_idx_t staffIdx)
{
    if (!system || staffIdx == muse::nidx || staffIdx >= system->staves().size()) {
        return 0.0;
    }

    return system->staff(staffIdx)->y();
}

double staffHeightForElement(const EngravingItem* item, double fallbackSpatium)
{
    const Staff* staff = item ? item->staff() : nullptr;
    return staff ? staff->staffHeight(item->tick()) : 4.0 * fallbackSpatium;
}

bool rectIsAboveStaff(const RectF& rect, const EngravingItem* item)
{
    const double spatium = item ? item->spatium() : 0.0;
    const Measure* measure = item ? item->findMeasure() : nullptr;
    const System* system = measure ? measure->system() : nullptr;
    const staff_idx_t staffIdx = item ? item->vStaffIdx() : muse::nidx;
    const double staffTop = staffYInSystem(system, staffIdx);
    const double staffBottom = staffTop + staffHeightForElement(item, spatium);
    return rect.center().y() < 0.5 * (staffTop + staffBottom);
}

Note::PianomaniaHand fingeredChordHand(const Chord* chord)
{
    if (!chord) {
        return Note::PianomaniaHand::Undefined;
    }

    Note::PianomaniaHand hand = Note::PianomaniaHand::Undefined;
    for (const Note* note : chord->notes()) {
        bool hasFingering = false;
        for (const EngravingItem* item : note->el()) {
            if (item && item->isFingering()) {
                hasFingering = true;
                break;
            }
        }
        if (!hasFingering) {
            continue;
        }

        const Note::PianomaniaHand noteHand = note->pianomaniaHand();
        if (noteHand == Note::PianomaniaHand::Undefined) {
            return Note::PianomaniaHand::Undefined;
        }
        if (hand == Note::PianomaniaHand::Undefined) {
            hand = noteHand;
        } else if (hand != noteHand) {
            return Note::PianomaniaHand::Undefined;
        }
    }

    return hand;
}

bool chordPitchRangesAreChordallyAdjacent(const Chord* a, const Chord* b)
{
    if (!a || !b || a->notes().empty() || b->notes().empty()) {
        return false;
    }

    constexpr int chordalAdjacentMaxPitchGap = 5;
    const int aLow = std::min(a->downNote()->pitch(), a->upNote()->pitch());
    const int aHigh = std::max(a->downNote()->pitch(), a->upNote()->pitch());
    const int bLow = std::min(b->downNote()->pitch(), b->upNote()->pitch());
    const int bHigh = std::max(b->downNote()->pitch(), b->upNote()->pitch());

    int gap = 0;
    if (aHigh < bLow) {
        gap = bLow - aHigh;
    } else if (bHigh < aLow) {
        gap = aLow - bHigh;
    }

    return gap <= chordalAdjacentMaxPitchGap;
}

// Union pitch range swept by one track's chords over a tick window. A voice that
// belongs to a block sonority stays put while it sounds; a running line (arpeggio,
// scale figure) sweeps a wide range and must keep voice-side digit placement even
// though one of its notes shares the attack tick.
bool trackPitchRangeInWindow(const Measure* measure, track_idx_t track, const Fraction& tick,
                             const Fraction& window, int& low, int& high)
{
    bool found = false;
    for (const Segment& s : measure->segments()) {
        if (!s.isChordRestType() || s.tick() < tick || s.tick() >= tick + window) {
            continue;
        }
        const EngravingItem* e = s.element(track);
        if (!e || !e->isChord()) {
            continue;
        }
        const Chord* c = toChord(e);
        if (c->notes().empty()) {
            continue;
        }
        const int cLow = std::min(c->downNote()->pitch(), c->upNote()->pitch());
        const int cHigh = std::max(c->downNote()->pitch(), c->upNote()->pitch());
        low = found ? std::min(low, cLow) : cLow;
        high = found ? std::max(high, cHigh) : cHigh;
        found = true;
    }
    return found;
}

bool pianomaniaSameHandFingeredChordalAttack(const Chord* chord, const Measure* measure)
{
    if (!chord || !measure || chord->isGrace()) {
        return false;
    }

    const Note::PianomaniaHand hand = fingeredChordHand(chord);
    if (hand == Note::PianomaniaHand::Undefined) {
        return false;
    }

    const Fraction tick = chord->tick();
    for (const Segment& s : measure->segments()) {
        if (!s.isChordRestType() || s.tick() != tick) {
            continue;
        }
        for (EngravingItem* e : s.elist()) {
            if (!e || !e->isChord() || e == chord) {
                continue;
            }
            const Chord* other = toChord(e);
            if (other->track() == chord->track() || other->vStaffIdx() != chord->vStaffIdx()
                || other->notes().empty()) {
                continue;
            }
            if (fingeredChordHand(other) != hand || !chordPitchRangesAreChordallyAdjacent(chord, other)) {
                continue;
            }
            // Chord components articulate on comparable time scales: a voice
            // sustained several times longer than its partner is a held bass
            // or pedal under a melody (Old French Song m1 half-vs-eighth),
            // not part of one struck chord — its digit stays on the voice side.
            const Fraction hostLen = chord->actualTicks();
            const Fraction otherLen = other->actualTicks();
            if (!(hostLen > Fraction(0, 1)) || !(otherLen > Fraction(0, 1))
                || std::max(hostLen, otherLen) > std::min(hostLen, otherLen) * 2) {
                continue;
            }
            // Both voices must hold still for as long as the sonority sounds:
            // over the longer chord's duration, each voice's swept range must
            // stay chord-sized. A voice that runs away (Arietta's LH sixteenth
            // arpeggio against its bass quarter) is a line, not a chord tone.
            constexpr int chordalCompactMaxSpan = 7;
            const Fraction window = std::max(chord->actualTicks(), other->actualTicks());
            int lowA = 0, highA = 0, lowB = 0, highB = 0;
            if (!trackPitchRangeInWindow(measure, chord->track(), tick, window, lowA, highA)
                || !trackPitchRangeInWindow(measure, other->track(), tick, window, lowB, highB)) {
                continue;
            }
            if (highA - lowA > chordalCompactMaxSpan || highB - lowB > chordalCompactMaxSpan) {
                continue;
            }
            return true;
        }
    }

    return false;
}

// In a staff carrying concurrent material from more than one voice, a digit
// belongs on its own voice's outer side — the side the stems point — so it
// stays next to its voice instead of being ejected past the other voice and
// the staff edge (upper voice → above, lower voice → below). The stem side
// is only trusted when it actually faces away from every concurrent
// other-voice chord on the displayed staff: divisi dyads written with
// matched stem directions and shared-notehead unisons keep the hand
// convention instead.
bool pianomaniaVoiceSideFingeringPlacement(const Chord* chord, PlacementV& placement)
{
    const Chord* host = chord;
    if (chord->isGrace()) {
        const EngravingObject* parent = chord->explicitParent();
        if (!parent || !parent->isChord()) {
            return false;
        }
        host = toChord(parent);
    }

    const Segment* segment = host->segment();
    const Measure* measure = segment ? segment->measure() : nullptr;
    if (!measure || chord->notes().empty()) {
        return false;
    }

    // A cross-staff-moved chord is a visitor on the displayed staff: its stem
    // direction encodes the beam geometry of the move, not a voice-side
    // convention. Engraved practice keeps its digits on the hand side, where
    // the segment-column stacking merges them with the host staff's digits
    // (e.g. the Arietta m7 "3/1" and "5/2" columns above the treble staff).
    if (host->staffMove() != 0 || chord->staffMove() != 0) {
        return false;
    }

    // When two same-hand, fingered voices attack together as one close
    // chordal sonority, the hand rule should win so the later segment-column
    // pass can merge the cross-voice digits into one stack.
    if (!chord->isGrace() && pianomaniaSameHandFingeredChordalAttack(host, measure)) {
        return false;
    }

    const Fraction tick = host->tick();
    Fraction len = host->actualTicks();
    if (!(len > Fraction(0, 1))) {
        len = Fraction(1, 8);
    }

    const double ownMid = 0.5 * (chord->upNote()->pitch() + chord->downNote()->pitch());
    const bool up = chord->up();
    bool concurrent = false;
    bool stemSideFacesAway = true;
    for (const Segment& s : measure->segments()) {
        if (!s.isChordRestType() || !(s.tick() < tick + len)) {
            continue;
        }
        for (EngravingItem* e : s.elist()) {
            if (!e || !e->isChord() || e == chord) {
                continue;
            }
            const Chord* other = toChord(e);
            if (other->track() == chord->track() || other->vStaffIdx() != chord->vStaffIdx()
                || other->notes().empty() || !(s.tick() + other->actualTicks() > tick)) {
                continue;
            }
            concurrent = true;
            const double otherMid = 0.5 * (other->upNote()->pitch() + other->downNote()->pitch());
            stemSideFacesAway = stemSideFacesAway && (up ? otherMid < ownMid : otherMid > ownMid);
        }
    }

    if (!concurrent || !stemSideFacesAway) {
        return false;
    }

    placement = up ? PlacementV::ABOVE : PlacementV::BELOW;
    return true;
}

PlacementV pianomaniaPreferredFingeringPlacement(const Fingering* fingering)
{
    const Note* note = fingering->note();
    const Chord* chord = note ? note->chord() : nullptr;
    const Staff* staff = chord ? chord->staff() : nullptr;
    const Part* part = staff ? staff->part() : nullptr;
    if (!note || !chord || !staff || !part) {
        return fingering->calculatePlacement();
    }

    PlacementV voiceSide = PlacementV::ABOVE;
    if (pianomaniaVoiceSideFingeringPlacement(chord, voiceSide)) {
        return voiceSide;
    }

    if (note->pianomaniaHand() == Note::PianomaniaHand::Left) {
        return PlacementV::BELOW;
    }
    if (note->pianomaniaHand() == Note::PianomaniaHand::Right) {
        return PlacementV::ABOVE;
    }

    const size_t staffCount = part->nstaves();
    if (staffCount > 1 && staff->rstaff() == staffCount - 1) {
        return PlacementV::BELOW;
    }

    return fingering->calculatePlacement();
}

PointF slurGripSystemPos(const SlurSegment* segment, Grip grip)
{
    return PointF(0.0, staffYInSystem(segment->system(), segment->vStaffIdx()))
           + segment->pos() + segment->ups(grip).pos();
}

struct SlurWindowSample {
    bool any = false;      // any curve sample within the horizontal window
    bool conflict = false; // any such sample within the vertical clearance band
    double minY = 0.0;     // highest curve point inside the window
    double maxY = 0.0;     // lowest curve point inside the window
};

SlurWindowSample sampleSlurWithinFingeringWindow(const RectF& fingeringRect, const SlurSegment* slurSegment,
                                                 double xMargin, double verticalClearance)
{
    SlurWindowSample sample;
    const PointF start = slurGripSystemPos(slurSegment, Grip::START);
    const PointF bezier1 = slurGripSystemPos(slurSegment, Grip::BEZIER1);
    const PointF bezier2 = slurGripSystemPos(slurSegment, Grip::BEZIER2);
    const PointF end = slurGripSystemPos(slurSegment, Grip::END);
    const double left = fingeringRect.left() - xMargin;
    const double right = fingeringRect.right() + xMargin;
    if (std::max(start.x(), end.x()) < left || std::min(start.x(), end.x()) > right) {
        return sample;
    }

    CubicBezier bezier(start, bezier1, bezier2, end);
    static constexpr int samples = 96;
    for (int i = 0; i <= samples; ++i) {
        const PointF point = bezier.pointAtPercent(double(i) / double(samples));
        if (point.x() < left || point.x() > right) {
            continue;
        }

        if (!sample.any) {
            sample.any = true;
            sample.minY = point.y();
            sample.maxY = point.y();
        } else {
            sample.minY = std::min(sample.minY, point.y());
            sample.maxY = std::max(sample.maxY, point.y());
        }
        if (point.y() > fingeringRect.top() - verticalClearance && point.y() < fingeringRect.bottom() + verticalClearance) {
            sample.conflict = true;
        }
    }

    return sample;
}

struct SlurAvoidance {
    bool conflict = false;
    double awayMove = 0.0;   // move away from the staff that clears every conflicting slur
    double towardMove = 0.0; // move toward the notes that ducks under every conflicting slur
};

SlurAvoidance slurAvoidanceForRect(const RectF& fingeringRect, const std::vector<FingeringObstacle>& obstacles,
                                   staff_idx_t staffIdx, double staffTop, bool above, double spatium,
                                   double slurClearanceSp = PM_FINGERING_SLUR_CLEARANCE)
{
    SlurAvoidance avoidance;
    const double xMargin = PM_FINGERING_SLUR_X_MARGIN * spatium;
    for (const FingeringObstacle& obstacle : obstacles) {
        // No nominal staff/side filtering for slurs: chords moved across
        // staves regularly put a bass-staff slur's arc over the treble staff
        // (and vice versa), so only the rendered geometry decides whether the
        // slur constrains this fingering.
        if (!obstacle.slurSegment || obstacle.slurSegment->ldata()->isSkipDraw()) {
            continue;
        }

        if (obstacle.staffIdx != staffIdx) {
            // Cross-staff geometry is only meaningful once per-staff system
            // offsets exist. Before SystemLayout::layout2 every SysStaff still
            // sits at y=0, which would paint another staff's slur arc into
            // this staff as a phantom obstacle. Same-staff slurs share the
            // fingering's frame, so any offset error cancels; cross-staff
            // slurs are re-checked by the page-stage rerun with real offsets.
            const double slurStaffY = staffYInSystem(obstacle.slurSegment->system(), obstacle.slurSegment->vStaffIdx());
            if (std::abs(slurStaffY - staffTop) < 0.01) {
                continue;
            }
        }

        const double clearance = slurClearanceSp * spatium + obstacle.slurSegment->ldata()->midThickness();
        const SlurWindowSample sample = sampleSlurWithinFingeringWindow(fingeringRect, obstacle.slurSegment, xMargin, clearance);
        if (!sample.conflict) {
            continue;
        }

        avoidance.conflict = true;
        const double target = clearance + PM_FINGERING_SLUR_TARGET_EXTRA * spatium;
        if (above) {
            avoidance.awayMove = std::max(avoidance.awayMove, fingeringRect.bottom() + target - sample.minY);
            avoidance.towardMove = std::max(avoidance.towardMove, sample.maxY + target - fingeringRect.top());
        } else {
            avoidance.awayMove = std::max(avoidance.awayMove, sample.maxY + target - fingeringRect.top());
            avoidance.towardMove = std::max(avoidance.towardMove, fingeringRect.bottom() + target - sample.minY);
        }
    }

    avoidance.awayMove = std::max(0.0, avoidance.awayMove);
    avoidance.towardMove = std::max(0.0, avoidance.towardMove);
    return avoidance;
}

double requiredVerticalMoveFromNotationRect(const RectF& fingeringRect, const RectF& notationRect, bool above, double clearance)
{
    if (notationRect.isNull()
        || fingeringRect.right() < notationRect.left() - clearance
        || fingeringRect.left() > notationRect.right() + clearance) {
        return 0.0;
    }

    if (above) {
        return std::max(0.0, fingeringRect.bottom() + clearance - notationRect.top());
    }
    return std::max(0.0, notationRect.bottom() + clearance - fingeringRect.top());
}

double requiredVerticalMoveFromMarkObstacles(const RectF& fingeringRect, const std::vector<FingeringObstacle>& obstacles,
                                             staff_idx_t staffIdx, bool above, double clearance)
{
    double required = 0.0;
    for (const FingeringObstacle& obstacle : obstacles) {
        if (obstacle.slurSegment || obstacle.dynamicSegment) {
            continue;
        }
        if (obstacle.staffIdx != staffIdx || obstacle.above != above) {
            continue;
        }
        required = std::max(required, requiredVerticalMoveFromNotationRect(fingeringRect, obstacle.rect, above, clearance));
    }
    return required;
}

bool tupletObstacleOverlapsFingeringXWindow(const RectF& fingeringRect, const std::vector<FingeringObstacle>& obstacles,
                                            staff_idx_t staffIdx, bool above, double clearance)
{
    for (const FingeringObstacle& obstacle : obstacles) {
        if (obstacle.kind != FingeringObstacleKind::Tuplet || obstacle.staffIdx != staffIdx || obstacle.above != above) {
            continue;
        }
        if (fingeringRect.right() >= obstacle.rect.left() - clearance
            && fingeringRect.left() <= obstacle.rect.right() + clearance) {
            return true;
        }
    }
    return false;
}

// First numeral of the fingering label ("5-3" → 5); -1 when the label
// carries no digit.
int pianomaniaFingeringDigit(const Fingering* fingering)
{
    const muse::String text = fingering->plainText();
    for (size_t i = 0; i < text.size(); ++i) {
        const muse::Char c = text.at(i);
        if (c.isDigit()) {
            return c.digitValue();
        }
    }
    return -1;
}

RectF fingeringSystemRect(const Fingering* fingering)
{
    const Note* note = fingering->note();
    const Chord* chord = note ? note->chord() : nullptr;
    const Segment* segment = chord ? chord->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    if (!note || !chord || !segment || !measure) {
        return RectF();
    }

    return fingering->ldata()->bbox().translated(PointF(0.0, staffYInSystem(measure->system(), chord->vStaffIdx()))
                                                 + fingering->pos() + note->pos() + chord->pos() + segment->pos()
                                                 + measure->pos());
}

void uniteRect(RectF& rect, const RectF& add)
{
    if (add.isNull()) {
        return;
    }
    if (rect.isNull()) {
        rect = add;
        return;
    }
    rect.unite(add);
}

RectF noteSystemRect(const Note* note)
{
    const Chord* chord = note ? note->chord() : nullptr;
    const Segment* segment = chord ? chord->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    if (!note || !chord || !segment || !measure) {
        return RectF();
    }

    return note->ldata()->bbox().translated(PointF(0.0, staffYInSystem(measure->system(), chord->vStaffIdx()))
                                            + note->pos() + chord->pos() + segment->pos() + measure->pos());
}

RectF fingeringGroupSystemRect(const std::vector<Fingering*>& fingerings);

bool beamIsCrossStaff(const Beam* beam)
{
    if (!beam) {
        return false;
    }

    bool crossBeam = beam->cross() || beam->fullCross();
    for (const ChordRest* cr : beam->elements()) {
        crossBeam = crossBeam || cr->staffMove() != 0;
    }
    return crossBeam;
}

void uniteVisibleBeamBoxesOverlappingX(RectF& danger, const Beam* beam, const System* system, double xLeft, double xRight)
{
    if (!beam || !beam->visible() || !beam->ldata() || beam->ldata()->isSkipDraw()) {
        return;
    }

    const double beamStaffY = staffYInSystem(system, beam->staffIdx());
    for (const BeamSegment* beamSegment : beam->beamSegments()) {
        for (const ShapeElement& box : beamSegment->shape().elements()) {
            if (box.left() > xRight || box.right() < xLeft) {
                continue;
            }
            uniteRect(danger, box.translated(0.0, beamStaffY));
        }
    }
}

void uniteSameStaffBeamDangerForGraceGroup(RectF& danger, const std::vector<Fingering*>& fingerings)
{
    if (fingerings.empty()) {
        return;
    }

    const Note* note = fingerings.front()->note();
    const Chord* chord = note ? note->chord() : nullptr;
    const Segment* segment = chord ? chord->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    if (!chord || !chord->isGrace() || !measure) {
        return;
    }

    const RectF digitRect = fingeringGroupSystemRect(fingerings);
    if (digitRect.isNull()) {
        return;
    }

    const staff_idx_t staffIdx = chord->vStaffIdx();
    std::set<const Beam*> seen;
    for (const Segment& measureSegment : measure->segments()) {
        if (!measureSegment.isChordRestType()) {
            continue;
        }
        for (EngravingItem* item : measureSegment.elist()) {
            if (!item || !item->isChord()) {
                continue;
            }
            const Chord* beamChord = toChord(item);
            const Beam* beam = beamChord->beam();
            if (beamChord->vStaffIdx() != staffIdx || !beam || beamIsCrossStaff(beam) || !seen.insert(beam).second) {
                continue;
            }
            if (!beam->visible() || !beam->ldata() || beam->ldata()->isSkipDraw()) {
                continue;
            }
            // A same-staff beam's segment shapes are already in the digit's
            // frame (unlike the cross-staff clause, which translates by
            // staffYInSystem). Unite only the sub-boxes under the digit's own
            // x-window: a sloped beam's whole bbox overstates the beam by the
            // slope drop across the group, which would wall off the pocket the
            // neighboring main-chord digits demonstrably occupy.
            const RectF beamRect = beam->ldata()->bbox();
            if (beamRect.left() > digitRect.right() || beamRect.right() < digitRect.left()) {
                continue;
            }
            bool united = false;
            for (const BeamSegment* beamSegment : beam->beamSegments()) {
                for (const ShapeElement& box : beamSegment->shape().elements()) {
                    if (box.left() <= digitRect.right() && box.right() >= digitRect.left()) {
                        uniteRect(danger, box);
                        united = true;
                    }
                }
            }
            if (!united) {
                // No segment geometry available (or none under the window):
                // fall back to the conservative whole-beam box.
                uniteRect(danger, beamRect);
            }
        }
    }
}

bool beamContainsGraceChord(const Beam* beam)
{
    if (!beam) {
        return false;
    }

    for (const ChordRest* cr : beam->elements()) {
        if (cr && cr->isChord() && toChord(cr)->isGrace()) {
            return true;
        }
    }
    return false;
}

void uniteSameStaffBeamBoxesOverlappingX(RectF& danger, const Beam* beam, const RectF& digitRect)
{
    if (!beam || !beam->visible() || !beam->ldata() || beam->ldata()->isSkipDraw() || digitRect.isNull()) {
        return;
    }

    // A same-staff beam's ldata bbox and segment shape boxes are already in
    // the digit/system frame. Do not translate them by staffYInSystem here.
    const RectF beamRect = beam->ldata()->bbox();
    if (beamRect.left() > digitRect.right() || beamRect.right() < digitRect.left()) {
        return;
    }

    bool united = false;
    for (const BeamSegment* beamSegment : beam->beamSegments()) {
        for (const ShapeElement& box : beamSegment->shape().elements()) {
            if (box.left() <= digitRect.right() && box.right() >= digitRect.left()) {
                uniteRect(danger, box);
                united = true;
            }
        }
    }
    if (!united) {
        uniteRect(danger, beamRect);
    }
}

void uniteNeighborGraceBeamDanger(RectF& danger, const std::vector<Fingering*>& fingerings)
{
    if (fingerings.empty()) {
        return;
    }

    const Note* note = fingerings.front()->note();
    const Chord* chord = note ? note->chord() : nullptr;
    const Segment* segment = chord ? chord->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    const System* system = measure ? measure->system() : nullptr;
    if (!chord || !measure) {
        return;
    }

    const RectF digitRect = fingeringGroupSystemRect(fingerings);
    if (digitRect.isNull()) {
        return;
    }

    const staff_idx_t staffIdx = chord->vStaffIdx();
    std::set<const Beam*> seen;
    for (const Segment& measureSegment : measure->segments()) {
        if (!measureSegment.isChordRestType()) {
            continue;
        }
        for (EngravingItem* item : measureSegment.elist()) {
            if (!item || !item->isChord()) {
                continue;
            }

            const Chord* beamChord = toChord(item);
            const Beam* beam = beamChord->beam();
            if (!beam || beamChord->vStaffIdx() != staffIdx || !beamContainsGraceChord(beam) || !seen.insert(beam).second) {
                continue;
            }

            if (beamIsCrossStaff(beam)) {
                uniteVisibleBeamBoxesOverlappingX(danger, beam, system, digitRect.left(), digitRect.right());
            } else {
                uniteSameStaffBeamBoxesOverlappingX(danger, beam, digitRect);
            }
        }
    }
}

RectF fingeringGroupNotationDangerRect(const std::vector<Fingering*>& fingerings, double xPad, bool includeSameStaffBeamsForGrace = false)
{
    RectF danger;
    for (const Fingering* fingering : fingerings) {
        const Note* note = fingering->note();
        uniteRect(danger, noteSystemRect(note));
    }

    // An eighth/sixteenth flag curls from the stem into the digit's column
    // below (down-stem) or above (up-stem) the notehead; a digit that only
    // clears the notehead lands right on it. Uniting the hook rect is
    // side-correct automatically: an up-stem hook never extends the danger
    // rect downward and vice versa. Like the cross-staff beam boxes below,
    // the hook only joins when it reaches over the fingered notehead's
    // center-x: an up-stem flag lives entirely on the stem side, and folding
    // it in would wall off the engraved notehead/stem pocket (the Arietta
    // m12/m22 "1" on the bass G nestles under its own flag).
    for (const Fingering* fingering : fingerings) {
        const Note* note = fingering->note();
        const Chord* chord = note ? note->chord() : nullptr;
        const Hook* hook = chord ? chord->hook() : nullptr;
        if (!hook || !hook->visible() || !hook->ldata() || hook->ldata()->isSkipDraw()) {
            continue;
        }
        const Segment* segment = chord->segment();
        const Measure* measure = segment ? segment->measure() : nullptr;
        if (!measure) {
            continue;
        }
        const RectF hookRect = hook->ldata()->bbox().translated(
            PointF(0.0, staffYInSystem(measure->system(), chord->vStaffIdx()))
            + hook->pos() + chord->pos() + segment->pos() + measure->pos());
        const double noteCenterX = noteSystemRect(note).center().x();
        if (hookRect.left() <= noteCenterX && hookRect.right() >= noteCenterX) {
            uniteRect(danger, hookRect);
        }
    }

    // Cross-staff beams are excluded from every skyline (BeamLayout::isTopBeam
    // rejects them), so the skyline descent checks cannot see them and a digit
    // can be dropped into the pocket between a notehead and its own beam. Fold
    // the beam boxes that pass under/over a fingered notehead into the danger
    // rect; beams that merely start beside the notehead (the digit's column is
    // clear) are left out, and skyline-visible beams are already handled.
    for (const Fingering* fingering : fingerings) {
        const Note* note = fingering->note();
        const Chord* chord = note ? note->chord() : nullptr;
        const Beam* beam = chord ? chord->beam() : nullptr;
        if (!beam || !beam->visible() || !beam->ldata() || beam->ldata()->isSkipDraw()) {
            continue;
        }
        if (!beamIsCrossStaff(beam)) {
            continue;
        }
        const Measure* measure = chord->findMeasure();
        const System* system = measure ? measure->system() : nullptr;
        const double noteCenterX = noteSystemRect(note).center().x();
        uniteVisibleBeamBoxesOverlappingX(danger, beam, system, noteCenterX, noteCenterX);
    }

    if (includeSameStaffBeamsForGrace) {
        uniteSameStaffBeamDangerForGraceGroup(danger, fingerings);
    }
    uniteNeighborGraceBeamDanger(danger, fingerings);

    // A stem pointing into the digit's side is a wall the digit may sit
    // BESIDE (m1's "2" in the up-stem pocket is engraved practice) but not
    // UNDER: include it only when it meaningfully x-overlaps the digit — in
    // practice wide multi-character labels like the m12/m22 "5-3" finger
    // switch, which the explicit down-stem would otherwise strike through.
    for (const Fingering* fingering : fingerings) {
        const Note* note = fingering->note();
        const Chord* chord = note ? note->chord() : nullptr;
        const Stem* stem = chord ? chord->stem() : nullptr;
        if (!stem || !stem->visible() || !stem->ldata() || stem->ldata()->isSkipDraw()) {
            continue;
        }
        const bool intoDigitSide = (fingering->placement() == PlacementV::ABOVE) == chord->up();
        if (!intoDigitSide) {
            continue;
        }
        const Segment* segment = chord->segment();
        const Measure* measure = segment ? segment->measure() : nullptr;
        if (!measure) {
            continue;
        }
        const RectF digitRect = fingeringSystemRect(fingering);
        const RectF stemRect = stem->ldata()->bbox().translated(
            PointF(0.0, staffYInSystem(measure->system(), chord->vStaffIdx()))
            + stem->pos() + chord->pos() + segment->pos() + measure->pos());
        // "Meaningfully overlaps" = the stem line runs THROUGH the label
        // (x-overlap alone maxes out at the stem's own hairline width), so a
        // digit whose edge merely touches the stem keeps its pocket.
        const double stemX = stemRect.center().x();
        const double margin = 0.15 * fingering->spatium();
        if (stemX > digitRect.left() + margin && stemX < digitRect.right() - margin) {
            uniteRect(danger, stemRect);
        }
    }

    // Vertical clearance is applied by the caller; only pad sideways so a
    // digit hovering just beside the notehead still counts as adjacent.
    return danger.adjusted(-xPad, 0.0, xPad, 0.0);
}

RectF fingeringGroupSystemRect(const std::vector<Fingering*>& fingerings)
{
    RectF rect;
    for (const Fingering* fingering : fingerings) {
        uniteRect(rect, fingeringSystemRect(fingering));
    }
    return rect;
}

Shape fingeringGroupSystemShape(const std::vector<Fingering*>& fingerings)
{
    Shape shape;
    for (const Fingering* fingering : fingerings) {
        const RectF rect = fingeringSystemRect(fingering);
        if (!rect.isNull()) {
            shape.add(ShapeElement(rect, fingering));
        }
    }
    return shape;
}

RectF fingeringGroupNoteSystemRect(const std::vector<Fingering*>& fingerings)
{
    RectF rect;
    for (const Fingering* fingering : fingerings) {
        uniteRect(rect, noteSystemRect(fingering->note()));
    }
    return rect;
}

RectF hairpinSegmentSystemRect(const SpannerSegment* segment)
{
    if (!segment || !segment->ldata()) {
        return RectF();
    }

    return segment->ldata()->bbox().translated(
        segment->pos() + PointF(0.0, staffYInSystem(segment->system(), segment->vStaffIdx())));
}

std::vector<TextHairpinObstacle> collectPianomaniaTextHairpinObstacles(System* system, staff_idx_t staffIdx)
{
    std::vector<TextHairpinObstacle> obstacles;
    if (!system || staffIdx == muse::nidx) {
        return obstacles;
    }

    std::set<const Beam*> seenBeams;
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* measure = toMeasure(mb);
        for (Segment& segment : measure->segments()) {
            if (!segment.isChordRestType()) {
                continue;
            }
            for (EngravingItem* item : segment.elist()) {
                if (!item || !item->isChord() || item->vStaffIdx() != staffIdx) {
                    continue;
                }

                Chord* chord = toChord(item);
                for (Note* note : chord->notes()) {
                    const RectF noteRect = noteSystemRect(note);
                    if (!noteRect.isNull()) {
                        obstacles.push_back(TextHairpinObstacle { noteRect });
                    }
                    for (EngravingItem* noteItem : note->el()) {
                        if (!noteItem || !noteItem->isFingering()) {
                            continue;
                        }
                        const RectF fingeringRect = fingeringSystemRect(toFingering(noteItem));
                        if (!fingeringRect.isNull()) {
                            obstacles.push_back(TextHairpinObstacle { fingeringRect });
                        }
                    }
                }

                const Stem* stem = chord->stem();
                if (stem && stem->visible() && stem->ldata() && !stem->ldata()->isSkipDraw()) {
                    const RectF stemRect = stem->ldata()->bbox().translated(
                        PointF(0.0, staffYInSystem(system, staffIdx))
                        + stem->pos() + chord->pos() + segment.pos() + measure->pos() + stem->staffOffset());
                    if (!stemRect.isNull()) {
                        obstacles.push_back(TextHairpinObstacle { stemRect });
                    }
                }

                const Beam* beam = chord->beam();
                if (!beam || !seenBeams.insert(beam).second) {
                    continue;
                }
                if (beamIsCrossStaff(beam)) {
                    RectF beamRect;
                    uniteVisibleBeamBoxesOverlappingX(beamRect, beam, system, -DBL_MAX, DBL_MAX);
                    if (!beamRect.isNull()) {
                        obstacles.push_back(TextHairpinObstacle { beamRect });
                    }
                } else if (beam->visible() && beam->ldata() && !beam->ldata()->isSkipDraw()) {
                    const RectF beamRect = beam->ldata()->bbox();
                    if (!beamRect.isNull()) {
                        obstacles.push_back(TextHairpinObstacle { beamRect });
                    }
                }
            }
        }
    }

    return obstacles;
}

bool centerFingeringGroupOverNotes(const std::vector<Fingering*>& fingerings)
{
    RectF fingeringRect = fingeringGroupSystemRect(fingerings);
    RectF noteRect = fingeringGroupNoteSystemRect(fingerings);
    if (fingeringRect.isNull() || noteRect.isNull()) {
        return false;
    }

    const double dx = noteRect.center().x() - fingeringRect.center().x();
    if (std::abs(dx) <= 0.05 * fingerings.front()->spatium()) {
        return false;
    }

    for (Fingering* fingering : fingerings) {
        fingering->mutldata()->moveX(dx);
    }
    return true;
}

double requiredVerticalMoveFromStaff(const RectF& fingeringRect, double staffTop, double staffBottom, bool above, double clearance)
{
    if (above) {
        return std::max(0.0, fingeringRect.bottom() + clearance - staffTop);
    }
    return std::max(0.0, staffBottom + clearance - fingeringRect.top());
}

struct FingeringGroupContext {
    const std::vector<FingeringObstacle>* obstacles = nullptr;
    const System* system = nullptr;
    RectF noteDangerRect;
    RectF noteheadRect;
    Shape groupShape;
    double staffTop = 0.0;
    double staffBottom = 0.0;
    staff_idx_t staffIdx = muse::nidx;
    bool above = true;
    double spatium = 1.0;
    // Grace-note digits never tuck under a slur: their pockets are cramped and
    // engraved sources keep the digit outside the slur.
    bool allowTuck = true;
    double maxDescent = 0.0; // skyline-safe room to move toward the notes from the current position
    // Same skyline measurement with a near-touch pad: how deep a tuck may
    // reach into the pocket before hitting anything the obstacle model
    // doesn't know about (stems, beams, accidentals, ties, ...).
    double maxTuckDescent = 0.0;
    // Extra allowance proven by a neighboring digit that tucked under the same
    // slur chain. Final note/mark/staff/slur and skyline-overlap checks still
    // decide whether this group may use it.
    double neighborTuckAllowance = 0.0;
};

bool fingeringTuckSkylineFilterOut(const ShapeElement& element)
{
    // Staff lines are excluded as well: whether a digit may cross into the
    // staff is decided by the placement rules (tuck staff intrusion), not by
    // the skyline; real obstacles inside the staff keep blocking.
    return element.item()
           && (element.item()->isFingering() || element.item()->isSlurSegment() || element.item()->isStaffLines());
}

bool rectsOverlap(const RectF& a, const RectF& b)
{
    return a.left() < b.right() && a.right() > b.left()
           && a.top() < b.bottom() && a.bottom() > b.top();
}

bool fingeringGroupFinalTuckClearsSkyline(const RectF& baseRect, const RectF& candidateRect,
                                          const FingeringGroupContext& ctx, double padSp = 0.02)
{
    if (!ctx.system || ctx.staffIdx == muse::nidx || ctx.staffIdx >= ctx.system->staves().size()) {
        return true;
    }

    const SysStaff* sysStaff = ctx.system->staff(ctx.staffIdx);
    if (!sysStaff) {
        return true;
    }

    Shape candidateShape = !ctx.groupShape.empty() ? ctx.groupShape : Shape(baseRect);
    candidateShape.translate(PointF(candidateRect.left() - baseRect.left(), candidateRect.top() - baseRect.top()));
    candidateShape.translate(PointF(0.0, -ctx.staffTop));

    const double pad = padSp * ctx.spatium;
    candidateShape.adjust(-pad, -pad, pad, pad);

    const SkylineLine filtered = ctx.above
                                 ? sysStaff->skyline().north().getFilteredCopy(fingeringTuckSkylineFilterOut)
                                 : sysStaff->skyline().south().getFilteredCopy(fingeringTuckSkylineFilterOut);

    for (const ShapeElement& candidate : candidateShape.elements()) {
        if (candidate.height() <= 0.0) {
            continue;
        }
        for (const ShapeElement& obstacle : filtered.elements()) {
            if (obstacle.height() <= 0.0) {
                continue;
            }
            if (rectsOverlap(candidate, obstacle)) {
                return false;
            }
        }
    }

    return true;
}

bool fingeringPlacementClearsOppositeSide(const RectF& groupRect, const GroupPlacement& placement,
                                          const FingeringGroupContext& ctx)
{
    const double sp = ctx.spatium;
    if (!placement.slurResolved) {
        return false;
    }

    const RectF finalRect = groupRect.translated(placement.dx, ctx.above ? -placement.moveAway : placement.moveAway);
    return requiredVerticalMoveFromNotationRect(finalRect, ctx.noteDangerRect, ctx.above,
                                                PM_FINGERING_NOTE_CLEARANCE_MIN * sp) <= 0.0
           && requiredVerticalMoveFromMarkObstacles(finalRect, *ctx.obstacles, ctx.staffIdx, ctx.above,
                                                    PM_FINGERING_MARK_CLEARANCE_MIN * sp) <= 0.0
           && !slurAvoidanceForRect(finalRect, *ctx.obstacles, ctx.staffIdx, ctx.staffTop, ctx.above, sp,
                                    PM_FINGERING_SLUR_CLEARANCE_TUCK).conflict
           && fingeringGroupFinalTuckClearsSkyline(groupRect, finalRect, ctx);
}

double fingeringRectDistanceFromNoteheads(const RectF& fingeringRect, const RectF& noteheadRect, bool above)
{
    if (fingeringRect.isNull() || noteheadRect.isNull()) {
        return 0.0;
    }

    const double distance = above ? noteheadRect.top() - fingeringRect.bottom()
                            : fingeringRect.top() - noteheadRect.bottom();
    return std::max(0.0, distance);
}

double fingeringPlacementDistanceFromNoteheads(const RectF& groupRect, const GroupPlacement& placement,
                                              const FingeringGroupContext& ctx)
{
    const RectF finalRect = groupRect.translated(placement.dx, ctx.above ? -placement.moveAway : placement.moveAway);
    return fingeringRectDistanceFromNoteheads(finalRect, ctx.noteheadRect, ctx.above);
}

bool fingeringPlacementExceedsNoteheadCap(const RectF& groupRect, const GroupPlacement& placement,
                                          const FingeringGroupContext& ctx)
{
    return fingeringPlacementDistanceFromNoteheads(groupRect, placement, ctx)
           > PM_FINGERING_NOTEHEAD_DETACHMENT_CAP * ctx.spatium;
}

bool manualFingeringGroupOverlapsNotation(const std::vector<Fingering*>& fingerings, bool graceGroup)
{
    if (fingerings.empty()) {
        return false;
    }

    const Fingering* first = fingerings.front();
    const Note* note = first->note();
    const Chord* chord = note ? note->chord() : nullptr;
    const Segment* segment = chord ? chord->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    const System* system = measure ? measure->system() : nullptr;
    if (!chord || !segment || !measure) {
        return false;
    }

    const double spatium = first->spatium();
    const RectF rect = fingeringGroupSystemRect(fingerings);
    if (rect.isNull()) {
        return false;
    }

    const RectF danger = fingeringGroupNotationDangerRect(fingerings, 0.2 * spatium, graceGroup);
    if (!danger.isNull() && rectsOverlap(rect, danger)) {
        return true;
    }

    FingeringGroupContext ctx;
    ctx.system = system;
    ctx.groupShape = fingeringGroupSystemShape(fingerings);
    ctx.staffIdx = chord->vStaffIdx();
    ctx.staffTop = staffYInSystem(system, ctx.staffIdx);
    ctx.above = first->placement() == PlacementV::ABOVE;
    ctx.spatium = spatium;
    return !fingeringGroupFinalTuckClearsSkyline(rect, rect, ctx, 0.0);
}

bool fingeringGroupPreferredSideTupletBlocked(const std::vector<Fingering*>& fingerings,
                                              const std::vector<FingeringObstacle>& obstacles)
{
    if (fingerings.empty()) {
        return false;
    }

    const Fingering* first = fingerings.front();
    const Note* note = first ? first->note() : nullptr;
    const Chord* chord = note ? note->chord() : nullptr;
    if (!first || !chord) {
        return false;
    }

    const RectF rect = fingeringGroupSystemRect(fingerings);
    if (rect.isNull()) {
        return false;
    }

    const double spatium = first->spatium();
    const double tupletClearance = PM_FINGERING_MARK_CLEARANCE * spatium;
    const bool above = pianomaniaPreferredFingeringPlacement(first) == PlacementV::ABOVE;
    for (const Fingering* fingering : fingerings) {
        const Note* fingeringNote = fingering ? fingering->note() : nullptr;
        const Chord* fingeringChord = fingeringNote ? fingeringNote->chord() : nullptr;
        Tuplet* tuplet = fingeringChord ? fingeringChord->tuplet() : nullptr;
        while (tuplet) {
            const RectF markerRect = tupletMarkerSystemRect(tuplet);
            if (!markerRect.isNull() && tuplet->vStaffIdx() == chord->vStaffIdx()
                && rectIsAboveStaff(markerRect, tuplet) == above
                && rect.right() >= markerRect.left() - tupletClearance
                && rect.left() <= markerRect.right() + tupletClearance) {
                return true;
            }
            tuplet = tuplet->tuplet();
        }
    }

    return tupletObstacleOverlapsFingeringXWindow(rect, obstacles, chord->vStaffIdx(), above, tupletClearance);
}

// How far the group may move toward the notes before hitting anything the
// obstacle model doesn't know about (stems, beams, accidentals, ties, ...),
// measured against the staff skyline. The group's own digits sit in the
// skyline at their pre-adjustment positions and slurs are handled by the
// dedicated sampler, so both are filtered out.
double fingeringGroupSkylineDescent(const std::vector<Fingering*>& fingerings, const System* system,
                                    staff_idx_t staffIdx, double staffTop, bool above, double spatium,
                                    double padSp = 0.2)
{
    if (!system || staffIdx == muse::nidx || staffIdx >= system->staves().size()) {
        return 0.0;
    }

    const SysStaff* sysStaff = system->staff(staffIdx);
    if (!sysStaff) {
        return 0.0;
    }

    Shape groupShape;
    for (const Fingering* fingering : fingerings) {
        const RectF rect = fingeringSystemRect(fingering);
        if (!rect.isNull()) {
            // The staff skyline lives in staff-relative coordinates.
            groupShape.add(ShapeElement(rect.translated(0.0, -staffTop)));
        }
    }
    if (groupShape.empty()) {
        return 0.0;
    }

    const double pad = padSp * spatium;
    double distance = 0.0;
    if (above) {
        const SkylineLine filtered = sysStaff->skyline().north().getFilteredCopy(fingeringTuckSkylineFilterOut);
        distance = filtered.minDistanceToShapeAbove(groupShape);
    } else {
        const SkylineLine filtered = sysStaff->skyline().south().getFilteredCopy(fingeringTuckSkylineFilterOut);
        distance = filtered.minDistanceToShapeBelow(groupShape);
    }

    return std::clamp(-distance - pad, 0.0, 4.0 * spatium);
}

// Resolves the vertical placement of a fingering group at a given horizontal
// shift. Notes, staff and marks (articulations etc.) push the group away from
// the staff; a conflicting slur is then resolved by preference: tuck the group
// into the pocket between the notes and the slur when the pocket fits, else
// climb just past the slur so the digits hug its far side.
GroupPlacement resolveFingeringGroupPlacement(const RectF& groupRect, double dx, const FingeringGroupContext& ctx)
{
    const double sp = ctx.spatium;
    auto movedAway = [&](const RectF& r, double away) {
        return r.translated(0.0, ctx.above ? -away : away);
    };

    RectF rect = groupRect.translated(dx, 0.0);
    const double base = std::max({
        requiredVerticalMoveFromNotationRect(rect, ctx.noteDangerRect, ctx.above, PM_FINGERING_NOTE_CLEARANCE * sp),
        requiredVerticalMoveFromStaff(rect, ctx.staffTop, ctx.staffBottom, ctx.above, PM_FINGERING_STAFF_CLEARANCE * sp),
        requiredVerticalMoveFromMarkObstacles(rect, *ctx.obstacles, ctx.staffIdx, ctx.above, PM_FINGERING_MARK_CLEARANCE * sp),
    });

    GroupPlacement placement;
    placement.dx = dx;
    double total = base;
    rect = movedAway(rect, base);

    // Vanilla autoplace often leaves a fingering higher above its note than
    // the obstacle model requires (skyline padding accumulates). When nothing
    // pushed the group away, walk it back toward the notes as far as both the
    // model and the skyline allow, so digits hug noteheads and marks.
    const double towardAllowance = std::max(PM_FINGERING_TUCK_MAX_BACKOFF * sp, ctx.maxDescent);
    if (muse::RealIsNull(dx) && base <= 0.0 && ctx.maxDescent > 0.0) {
        auto fitsAt = [&](double drop) {
            const RectF candidate = movedAway(rect, -drop);
            return requiredVerticalMoveFromNotationRect(candidate, ctx.noteDangerRect, ctx.above,
                                                        PM_FINGERING_NOTE_CLEARANCE * sp) <= 0.0
                   && requiredVerticalMoveFromStaff(candidate, ctx.staffTop, ctx.staffBottom, ctx.above,
                                                    PM_FINGERING_STAFF_CLEARANCE * sp) <= 0.0
                   && requiredVerticalMoveFromMarkObstacles(candidate, *ctx.obstacles, ctx.staffIdx, ctx.above,
                                                            PM_FINGERING_MARK_CLEARANCE * sp) <= 0.0
                   && !slurAvoidanceForRect(candidate, *ctx.obstacles, ctx.staffIdx, ctx.staffTop, ctx.above, sp).conflict;
        };
        if (fitsAt(0.0)) {
            double lo = 0.0;
            double hi = ctx.maxDescent;
            if (fitsAt(hi)) {
                lo = hi;
            } else {
                for (int i = 0; i < 8; ++i) {
                    const double mid = 0.5 * (lo + hi);
                    (fitsAt(mid) ? lo : hi) = mid;
                }
            }
            if (lo > 0.1 * sp) {
                total -= lo;
                rect = movedAway(rect, -lo);
            }
        }
    }

    for (int iter = 0; iter < 4; ++iter) {
        const SlurAvoidance avoidance = slurAvoidanceForRect(rect, *ctx.obstacles, ctx.staffIdx, ctx.staffTop, ctx.above, sp);
        if (!avoidance.conflict) {
            break;
        }

        // Tuck attempt: duck under the slur at the near-touch clearance. The
        // guaranteed-safe backoff below the vanilla baseline is small, but a
        // deeper reach into the pocket is allowed when the skyline proves it
        // clear of unmodeled obstacles (stems, beams, accidentals, ties).
        const double localTuckAllowance = std::max(towardAllowance, ctx.maxTuckDescent);
        const double tuckAllowance = std::max(localTuckAllowance, ctx.neighborTuckAllowance);
        const double tuckMove = slurAvoidanceForRect(rect, *ctx.obstacles, ctx.staffIdx, ctx.staffTop, ctx.above, sp,
                                                     PM_FINGERING_SLUR_CLEARANCE_TUCK).towardMove;
        GroupPlacement climbed;
        climbed.dx = dx;
        climbed.moveAway = total + avoidance.awayMove;
        climbed.slurResolved = true;
        const bool climbWouldExceedNoteheadCap = avoidance.awayMove > 0.0
                                                 && fingeringPlacementExceedsNoteheadCap(groupRect, climbed, ctx);
        if (iter == 0 && ctx.allowTuck && (total - tuckMove >= -tuckAllowance || climbWouldExceedNoteheadCap)) {
            const RectF tucked = movedAway(rect, -tuckMove);
            const bool clearOfNotes = requiredVerticalMoveFromNotationRect(tucked, ctx.noteDangerRect, ctx.above,
                                                                           PM_FINGERING_NOTE_CLEARANCE_MIN * sp) <= 0.0;
            const double staffIntrusion = PM_FINGERING_TUCK_STAFF_INTRUSION * sp;
            const bool clearOfStaff = requiredVerticalMoveFromStaff(tucked, ctx.staffTop + staffIntrusion,
                                                                    ctx.staffBottom - staffIntrusion, ctx.above,
                                                                    PM_FINGERING_STAFF_CLEARANCE_MIN * sp) <= 0.0;
            const bool clearOfMarks = requiredVerticalMoveFromMarkObstacles(tucked, *ctx.obstacles, ctx.staffIdx, ctx.above,
                                                                            PM_FINGERING_MARK_CLEARANCE_TUCK * sp) <= 0.0;
            const bool clearOfSlurs = !slurAvoidanceForRect(tucked, *ctx.obstacles, ctx.staffIdx, ctx.staffTop, ctx.above, sp,
                                                            PM_FINGERING_SLUR_CLEARANCE_TUCK).conflict;
            // Neighbor-assisted tucks may cross a one-way skyline descent
            // barrier, but the final rect still must not land on skyline
            // obstacles in this group's own x-window.
            const bool clearOfSkyline = fingeringGroupFinalTuckClearsSkyline(groupRect, tucked, ctx);
            if (clearOfNotes && clearOfStaff && clearOfMarks && clearOfSlurs && clearOfSkyline) {
                placement.moveAway = total - tuckMove;
                placement.tucked = true;
                placement.tuckAllowanceUsed = std::max(0.0, tuckMove - total);
                placement.tuckAllowanceProven = std::max({
                    localTuckAllowance,
                    ctx.neighborTuckAllowance,
                    placement.tuckAllowanceUsed
                });
                return placement;
            }
        }

        if (avoidance.awayMove <= 0.0) {
            break;
        }
        total += avoidance.awayMove;
        rect = movedAway(rect, avoidance.awayMove);
    }

    placement.moveAway = total;
    placement.slurResolved = !slurAvoidanceForRect(rect, *ctx.obstacles, ctx.staffIdx, ctx.staffTop, ctx.above, sp).conflict;
    return placement;
}

GroupPlacement chooseFingeringGroupPlacement(const RectF& groupRect, const FingeringGroupContext& ctx)
{
    const double sp = ctx.spatium;
    GroupPlacement centered = resolveFingeringGroupPlacement(groupRect, 0.0, ctx);
    if (centered.moveAway <= PM_FINGERING_DESPERATE_MOVE * sp) {
        return centered;
    }

    const std::array<double, 12> xCandidates = { -0.5 * sp, 0.5 * sp, -1.0 * sp, 1.0 * sp,
                                                 -1.5 * sp, 1.5 * sp, -2.0 * sp, 2.0 * sp,
                                                 -2.5 * sp, 2.5 * sp, -3.0 * sp, 3.0 * sp };

    GroupPlacement best = centered;
    double bestScore = centered.moveAway;
    for (double dx : xCandidates) {
        GroupPlacement candidate = resolveFingeringGroupPlacement(groupRect, dx, ctx);
        const double score = candidate.moveAway + (std::abs(dx) * 1.75);
        if (score < bestScore) {
            best = candidate;
            bestScore = score;
        }
    }

    const double minimumHorizontalImprovement = 0.35 * sp;
    if (centered.moveAway - best.moveAway < minimumHorizontalImprovement) {
        return centered;
    }

    return best;
}

void normalizeManualFingeringIfRequested(Fingering* fingering)
{
    if (!MScore::pianomaniaForceNormalizeSlursFingerings || !hasManualFingeringPlacement(fingering)) {
        return;
    }

    fingering->undoChangeProperty(Pid::PLACEMENT, pianomaniaPreferredFingeringPlacement(fingering), PropertyFlags::STYLED);
    fingering->undoChangeProperty(Pid::OFFSET, PointF(), PropertyFlags::STYLED);
    fingering->undoChangeProperty(Pid::MIN_DISTANCE, fingering->propertyDefault(Pid::MIN_DISTANCE), PropertyFlags::STYLED);
    fingering->undoChangeProperty(Pid::AUTOPLACE, true);
    TLayout::layoutFingering(fingering, fingering->mutldata());
    ++MScore::pianomaniaNormalizedManualFingerings;
}

void applyPianomaniaPreferredFingeringPlacement(Fingering* fingering)
{
    if (hasManualFingeringPlacement(fingering) && !MScore::pianomaniaForceNormalizeSlursFingerings) {
        return;
    }

    const PlacementV placement = pianomaniaPreferredFingeringPlacement(fingering);
    if (fingering->placement() == placement) {
        return;
    }

    fingering->setPlacement(placement);
    TLayout::layoutFingering(fingering, fingering->mutldata());
}

bool visibleObstacleItem(const EngravingItem* item)
{
    return item && item->visible() && item->ldata() && !item->ldata()->isSkipDraw();
}

RectF chordAttachedItemSystemRect(const Chord* chord, const EngravingItem* item)
{
    const Segment* segment = chord ? chord->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    if (!chord || !item || !segment || !measure) {
        return RectF();
    }

    return item->shape().bbox().translated(PointF(0.0, staffYInSystem(measure->system(), chord->vStaffIdx()))
                                           + item->pos() + chord->pos() + segment->pos() + measure->pos()
                                           + chord->staffOffset());
}

RectF segmentAnnotationSystemRect(const EngravingItem* item)
{
    const EngravingObject* parent = item ? item->explicitParent() : nullptr;
    const Segment* segment = parent && parent->isSegment() ? toSegment(parent) : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    if (!item || !segment || !measure) {
        return RectF();
    }

    return item->shape().bbox().translated(PointF(0.0, staffYInSystem(measure->system(), item->vStaffIdx()))
                                           + item->pos() + segment->pos() + measure->pos() + item->staffOffset());
}

RectF measureElementSystemRect(const EngravingItem* item)
{
    const Measure* measure = item && item->explicitParent() && item->explicitParent()->isMeasure()
                             ? toMeasure(item->explicitParent()) : nullptr;
    if (!item || !measure) {
        return RectF();
    }

    return item->shape().bbox().translated(PointF(0.0, staffYInSystem(measure->system(), item->vStaffIdx()))
                                           + measure->pos() + item->pos() + item->staffOffset());
}

RectF tupletMarkerSystemRect(const Tuplet* tuplet)
{
    const Measure* measure = tuplet && tuplet->explicitParent() && tuplet->explicitParent()->isMeasure()
                             ? toMeasure(tuplet->explicitParent()) : nullptr;
    if (!tuplet || !measure) {
        return RectF();
    }

    if (const Text* number = tuplet->number(); number && number->ldata()) {
        return number->ldata()->bbox().translated(PointF(0.0, staffYInSystem(measure->system(), tuplet->vStaffIdx()))
                                                  + measure->pos() + tuplet->pos() + number->pos()
                                                  + tuplet->staffOffset());
    }

    return measureElementSystemRect(tuplet);
}

RectF chordRestSystemRect(const ChordRest* cr)
{
    const Segment* segment = cr ? cr->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    if (!cr || !segment || !measure) {
        return RectF();
    }

    return cr->shape().bbox().translated(PointF(0.0, staffYInSystem(measure->system(), cr->vStaffIdx()))
                                         + cr->pos() + segment->pos() + measure->pos() + cr->staffOffset());
}

void addRectObstacle(std::vector<FingeringObstacle>& obstacles, FingeringObstacleKind kind, const RectF& rect,
                     staff_idx_t staffIdx, bool above)
{
    if (rect.isNull() || staffIdx == muse::nidx) {
        return;
    }

    FingeringObstacle obstacle;
    obstacle.kind = kind;
    obstacle.rect = rect;
    obstacle.staffIdx = staffIdx;
    obstacle.above = above;
    obstacles.push_back(obstacle);
}

std::vector<FingeringObstacle> collectPianomaniaFingeringObstacles(System* system)
{
    std::vector<FingeringObstacle> obstacles;
    if (!system) {
        return obstacles;
    }

    for (SpannerSegment* spannerSegment : system->spannerSegments()) {
        if (spannerSegment && spannerSegment->isSlurSegment()) {
            FingeringObstacle obstacle;
            obstacle.kind = FingeringObstacleKind::Slur;
            obstacle.slurSegment = toSlurSegment(spannerSegment);
            obstacle.staffIdx = obstacle.slurSegment->vStaffIdx();
            obstacles.push_back(obstacle);
        }
    }

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* measure = toMeasure(mb);
        for (Segment& segment : measure->segments()) {
            if (segment.isChordRestType()) {
                for (EngravingItem* item : segment.elist()) {
                    if (!item || !item->isChordRest()) {
                        continue;
                    }
                    if (item->isRest()) {
                        const Rest* rest = toRest(item);
                        if (visibleObstacleItem(rest) && !rest->isGap()) {
                            const RectF rect = chordRestSystemRect(rest);
                            addRectObstacle(obstacles, FingeringObstacleKind::Rest, rect, rest->vStaffIdx(),
                                            rectIsAboveStaff(rect, rest));
                        }
                        continue;
                    }
                    if (!item->isChord()) {
                        continue;
                    }
                    Chord* chord = toChord(item);
                    for (Articulation* articulation : chord->articulations()) {
                        if (!visibleObstacleItem(articulation)) {
                            continue;
                        }
                        addRectObstacle(obstacles, FingeringObstacleKind::Mark, chordAttachedItemSystemRect(chord, articulation),
                                        chord->vStaffIdx(), articulation->up());
                    }
                    Ornament* ornament = chord->findOrnament();
                    if (visibleObstacleItem(ornament)
                        && !muse::contains(chord->articulations(), static_cast<Articulation*>(ornament))) {
                        addRectObstacle(obstacles, FingeringObstacleKind::Mark, chordAttachedItemSystemRect(chord, ornament),
                                        chord->vStaffIdx(), ornament->up());
                    }
                    TremoloSingleChord* tremolo = chord->tremoloSingleChord();
                    if (visibleObstacleItem(tremolo)) {
                        RectF rect = chordAttachedItemSystemRect(chord, tremolo);
                        addRectObstacle(obstacles, FingeringObstacleKind::Mark, rect, chord->vStaffIdx(),
                                        rectIsAboveStaff(rect, tremolo));
                    }
                }
            }

            for (EngravingItem* annotation : segment.annotations()) {
                if (!visibleObstacleItem(annotation)) {
                    continue;
                }
                if (annotation->isFermata()) {
                    addRectObstacle(obstacles, FingeringObstacleKind::Mark, segmentAnnotationSystemRect(annotation),
                                    annotation->vStaffIdx(), annotation->placeAbove());
                } else if (annotation->isTremoloBar()) {
                    RectF rect = segmentAnnotationSystemRect(annotation);
                    addRectObstacle(obstacles, FingeringObstacleKind::Mark, rect, annotation->vStaffIdx(),
                                    rectIsAboveStaff(rect, annotation));
                } else if (annotation->isDynamic()
                           && isPianomaniaNoteModifierDynamic(toDynamic(annotation)->dynamicType())) {
                    // The dynamic is laid out and aligned only after this
                    // pass, so reserve its natural landing zone: centered on
                    // its chord, tight against the staff (or the chord, when
                    // the chord pokes out further).
                    const Dynamic* dynamic = toDynamic(annotation);
                    const EngravingItem* beatItem = segment.element(dynamic->track());
                    const Chord* chord = beatItem && beatItem->isChord() ? toChord(beatItem) : nullptr;
                    if (!chord) {
                        continue;
                    }
                    RectF notesRect;
                    for (const Note* note : chord->notes()) {
                        uniteRect(notesRect, noteSystemRect(note));
                    }
                    if (notesRect.isNull()) {
                        continue;
                    }
                    const double spatium = dynamic->spatium();
                    const double staffTop = staffYInSystem(system, dynamic->vStaffIdx());
                    const double staffBottom = staffTop + staffHeightForElement(dynamic, spatium);
                    const RectF dynBBox = dynamic->ldata() ? dynamic->ldata()->bbox() : RectF();
                    const double halfWidth = std::max(0.5 * dynBBox.width(), 1.0 * spatium);
                    const double height = std::max(dynBBox.height(), 1.2 * spatium) + 0.5 * spatium;
                    const bool above = dynamic->placeAbove();
                    const double edge = above ? std::min(staffTop, notesRect.top())
                                        : std::max(staffBottom, notesRect.bottom());
                    FingeringObstacle zone;
                    zone.kind = FingeringObstacleKind::DynamicZone;
                    zone.dynamicSegment = &segment;
                    zone.staffIdx = dynamic->vStaffIdx();
                    zone.above = above;
                    zone.rect = RectF(notesRect.center().x() - halfWidth, above ? edge - height : edge,
                                      2.0 * halfWidth, height);
                    obstacles.push_back(zone);
                }
            }
        }

        std::set<Tuplet*> seenTuplets;
        for (Segment& segment : measure->segments()) {
            if (!segment.isChordRestType()) {
                continue;
            }
            for (EngravingItem* item : segment.elist()) {
                if (!item || !item->isChordRest()) {
                    continue;
                }
                Tuplet* tuplet = toChordRest(item)->tuplet();
                while (tuplet) {
                    if (seenTuplets.insert(tuplet).second && visibleObstacleItem(tuplet)) {
                        const RectF rect = tupletMarkerSystemRect(tuplet);
                        addRectObstacle(obstacles, FingeringObstacleKind::Tuplet, rect, tuplet->vStaffIdx(),
                                        rectIsAboveStaff(rect, tuplet));
                    }
                    tuplet = tuplet->tuplet();
                }
            }
        }
    }

    return obstacles;
}

// Re-lays a fingering group on the given side. Vanilla stacking of multiple
// digits on one chord comes from the skyline, which is stale by this point in
// the pass, so the stack is rebuilt manually preserving note order (the digit
// of the outermost note sits outermost).
void setFingeringGroupPlacementAndRelayout(const std::vector<Fingering*>& fingerings, PlacementV placement)
{
    for (Fingering* fingering : fingerings) {
        fingering->setPlacement(placement);
        TLayout::layoutFingering(fingering, fingering->mutldata());
    }

    if (fingerings.size() < 2) {
        return;
    }

    std::vector<Fingering*> ordered = fingerings;
    std::sort(ordered.begin(), ordered.end(), [](const Fingering* a, const Fingering* b) {
        return noteSystemRect(a->note()).center().y() < noteSystemRect(b->note()).center().y();
    });
    const bool above = placement == PlacementV::ABOVE;
    if (above) {
        // Anchor on the lowest note's digit and stack upward.
        std::reverse(ordered.begin(), ordered.end());
    }
    // Engraved digit columns read by finger number: the larger number always
    // sits farther from the staff, even when the voices' pitches say
    // otherwise (Arietta m9/m19: melody 1 over moved-chord 4 must print as
    // "4 over 1"). Since the front entry anchors nearest the staff on both
    // sides, that is simply an ascending stable re-sort; digit-less or tied
    // labels keep the pitch order.
    std::stable_sort(ordered.begin(), ordered.end(), [](const Fingering* a, const Fingering* b) {
        const int da = pianomaniaFingeringDigit(a);
        const int db = pianomaniaFingeringDigit(b);
        if (da < 0 || db < 0) {
            return false;
        }
        return da < db;
    });

    // Compact the column in both directions: a digit that vanilla autoplace
    // lofted high above the anchor (skyline push past a slur or a published
    // rect) is pulled back down just as an overlapping digit is lifted — the
    // group-level resolve that follows moves the whole column as one unit, so
    // per-digit clearance is re-established there.
    const double gap = 0.25 * fingerings.front()->spatium();
    const RectF anchorRect = fingeringSystemRect(ordered.front());
    RectF previous = anchorRect;
    for (size_t i = 1; i < ordered.size(); ++i) {
        Fingering* fingering = ordered[i];
        RectF rect = fingeringSystemRect(fingering);
        // Engraved stacks read as one column: digits from different voices keep
        // their own noteheads' x, so align them to the anchor digit before the
        // group is centered over the note union as a whole.
        const double dx = anchorRect.center().x() - rect.center().x();
        const double dy = above ? previous.top() - gap - rect.bottom()
                          : previous.bottom() + gap - rect.top();
        if (!muse::RealIsNull(dx) || !muse::RealIsNull(dy)) {
            fingering->mutldata()->moveX(dx);
            fingering->mutldata()->moveY(dy);
            rect.translate(dx, dy);
        }
        previous = rect;
    }
}

struct PmFingeringGroupAdjustment {
    std::vector<Fingering*> fingerings;
    const Chord* chord = nullptr;
    Segment* segment = nullptr;
    FingeringGroupContext ctx;
    RectF baseRect;
    GroupPlacement chosen;
    bool allowTuck = true;
    bool flipped = false;
    bool valid = false;
};

bool adjustFingeringGroupAroundNotation(const std::vector<Fingering*>& fingerings, const std::vector<FingeringObstacle>& obstacles,
                                        bool allowOppositeSide, bool allowTuck, double neighborTuckAllowance = 0.0,
                                        PmFingeringGroupAdjustment* adjustment = nullptr, bool rescueManualPlacement = false)
{
    if (adjustment) {
        *adjustment = PmFingeringGroupAdjustment();
    }
    if (fingerings.empty()) {
        return false;
    }

    bool manual = false;
    for (const Fingering* fingering : fingerings) {
        manual = manual || hasManualFingeringPlacement(fingering);
    }
    if (manual && !MScore::pianomaniaForceNormalizeSlursFingerings && !rescueManualPlacement) {
        return false;
    }

    Fingering* first = fingerings.front();
    const Note* note = first->note();
    const Chord* chord = note ? note->chord() : nullptr;
    if (!chord) {
        return false;
    }

    const PlacementV preferredPlacement = pianomaniaPreferredFingeringPlacement(first);
    const staff_idx_t staffIdx = chord->vStaffIdx();
    const double spatium = first->spatium();
    const Staff* staff = first->staff();
    const Segment* segment = chord->segment();
    const Measure* measure = segment ? segment->measure() : nullptr;
    const System* system = measure ? measure->system() : nullptr;
    const double staffHeight = staff ? staff->staffHeight(first->tick()) : 4.0 * spatium;

    FingeringGroupContext ctx;
    ctx.obstacles = &obstacles;
    ctx.system = system;
    ctx.noteDangerRect = fingeringGroupNotationDangerRect(fingerings, 0.2 * spatium, chord->isGrace());
    ctx.noteheadRect = fingeringGroupNoteSystemRect(fingerings);
    ctx.groupShape = fingeringGroupSystemShape(fingerings);
    ctx.staffTop = staffYInSystem(system, staffIdx);
    ctx.staffBottom = ctx.staffTop + staffHeight;
    ctx.staffIdx = staffIdx;
    ctx.above = preferredPlacement == PlacementV::ABOVE;
    ctx.spatium = spatium;
    ctx.allowTuck = allowTuck;
    ctx.maxDescent = fingeringGroupSkylineDescent(fingerings, system, staffIdx, ctx.staffTop, ctx.above, spatium);
    ctx.maxTuckDescent = fingeringGroupSkylineDescent(fingerings, system, staffIdx, ctx.staffTop, ctx.above, spatium, 0.02);
    ctx.neighborTuckAllowance = neighborTuckAllowance;

    RectF rect = fingeringGroupSystemRect(fingerings);
    if (rect.isNull()) {
        return false;
    }

    // A note-modifier dynamic (sf family) owns the tight spot against this
    // chord. Instead of taking that spot (and pushing the dynamic away when
    // it lays out after this pass), the digits go to the LEFT of the reserved
    // zone at their natural height.
    GroupPlacement chosen;
    bool reservedZoneDodge = false;
    for (const FingeringObstacle& obstacle : obstacles) {
        if (obstacle.dynamicSegment != segment || obstacle.staffIdx != staffIdx || obstacle.above != ctx.above) {
            continue;
        }
        const double pad = 0.2 * spatium;
        if (rect.right() <= obstacle.rect.left() - pad || rect.left() >= obstacle.rect.right() + pad) {
            continue;
        }
        const double dxLeft = (obstacle.rect.left() - 0.25 * spatium) - rect.right();
        if (dxLeft < 0.0) {
            chosen = resolveFingeringGroupPlacement(rect, dxLeft, ctx);
            reservedZoneDodge = true;
        }
        break;
    }
    if (!reservedZoneDodge) {
        chosen = chooseFingeringGroupPlacement(rect, ctx);
    }
    bool flipped = false;

    const RectF preferredSideRect = rect.translated(chosen.dx, 0.0);
    const bool preferredSideTupletBlocked
        = allowOppositeSide && !reservedZoneDodge
          && (tupletObstacleOverlapsFingeringXWindow(preferredSideRect, obstacles, staffIdx, ctx.above,
                                                        PM_FINGERING_MARK_CLEARANCE * spatium)
              || fingeringGroupPreferredSideTupletBlocked(fingerings, obstacles));
    const bool belowSideNeedsGeneralFlip = !ctx.above && chosen.moveAway > PM_FINGERING_FLIP_TRIGGER * spatium;
    const bool preferredSideExceedsNoteheadCap = allowOppositeSide && !reservedZoneDodge
                                                && fingeringPlacementExceedsNoteheadCap(rect, chosen, ctx);

    // A below-preferred group whose own side costs a lot of vertical stacking
    // (e.g. squeezed by a down-arcing slur) may sit above the notes instead,
    // where the displacement is small. Any preferred-side placement that
    // detaches beyond the notehead cap may also use the opposite side, but
    // only when that side passes the same full collision gate as tuplet flips.
    if (allowOppositeSide && !reservedZoneDodge
        && (belowSideNeedsGeneralFlip || preferredSideTupletBlocked || preferredSideExceedsNoteheadCap)) {
        const PlacementV oppositePlacement = ctx.above ? PlacementV::BELOW : PlacementV::ABOVE;
        setFingeringGroupPlacementAndRelayout(fingerings, oppositePlacement);
        centerFingeringGroupOverNotes(fingerings);
        FingeringGroupContext flipCtx = ctx;
        flipCtx.above = oppositePlacement == PlacementV::ABOVE;
        flipCtx.noteDangerRect = fingeringGroupNotationDangerRect(fingerings, 0.2 * spatium, chord->isGrace());
        flipCtx.noteheadRect = fingeringGroupNoteSystemRect(fingerings);
        flipCtx.groupShape = fingeringGroupSystemShape(fingerings);
        flipCtx.neighborTuckAllowance = 0.0;
        flipCtx.maxDescent = fingeringGroupSkylineDescent(fingerings, system, staffIdx, ctx.staffTop, flipCtx.above, spatium);
        flipCtx.maxTuckDescent = fingeringGroupSkylineDescent(fingerings, system, staffIdx, ctx.staffTop, flipCtx.above,
                                                              spatium, 0.02);
        const RectF flipRect = fingeringGroupSystemRect(fingerings);
        const GroupPlacement alternative = resolveFingeringGroupPlacement(flipRect, 0.0, flipCtx);
        const bool acceptTupletFlip = preferredSideTupletBlocked
                                      && fingeringPlacementClearsOppositeSide(flipRect, alternative, flipCtx);
        const bool acceptGeneralFlip = belowSideNeedsGeneralFlip && !ctx.above && alternative.slurResolved
                                       && alternative.moveAway + PM_FINGERING_FLIP_ADVANTAGE * spatium < chosen.moveAway;
        const bool acceptDetachedFlip = preferredSideExceedsNoteheadCap
                                        && fingeringPlacementClearsOppositeSide(flipRect, alternative, flipCtx)
                                        && fingeringPlacementDistanceFromNoteheads(flipRect, alternative, flipCtx)
                                        <= PM_FINGERING_NOTEHEAD_DETACHMENT_CAP * spatium;
        if (acceptTupletFlip || acceptGeneralFlip || acceptDetachedFlip) {
            chosen = alternative;
            rect = flipRect;
            ctx = flipCtx;
            flipped = true;
        } else {
            setFingeringGroupPlacementAndRelayout(fingerings, preferredPlacement);
            centerFingeringGroupOverNotes(fingerings);
            rect = fingeringGroupSystemRect(fingerings);
            ctx.noteDangerRect = fingeringGroupNotationDangerRect(fingerings, 0.2 * spatium, chord->isGrace());
            ctx.noteheadRect = fingeringGroupNoteSystemRect(fingerings);
            ctx.groupShape = fingeringGroupSystemShape(fingerings);
            ctx.maxDescent = fingeringGroupSkylineDescent(fingerings, system, staffIdx, ctx.staffTop, ctx.above, spatium);
            ctx.maxTuckDescent = fingeringGroupSkylineDescent(fingerings, system, staffIdx, ctx.staffTop, ctx.above, spatium, 0.02);
            chosen = chooseFingeringGroupPlacement(rect, ctx);
        }
    }

    if (!flipped && muse::RealIsNull(chosen.moveAway) && muse::RealIsNull(chosen.dx)) {
        return false;
    }

    const double dy = ctx.above ? -chosen.moveAway : chosen.moveAway;
    for (Fingering* fingering : fingerings) {
        fingering->mutldata()->moveX(chosen.dx);
        fingering->mutldata()->moveY(dy);
    }

    const RectF finalRect = rect.translated(chosen.dx, dy);
    const bool notationStillCollides
        = requiredVerticalMoveFromMarkObstacles(finalRect, obstacles, ctx.staffIdx, ctx.above,
                                                PM_FINGERING_MARK_CLEARANCE_TUCK * spatium) > 0.0
          || slurAvoidanceForRect(finalRect, obstacles, ctx.staffIdx, ctx.staffTop, ctx.above, spatium,
                                  PM_FINGERING_SLUR_CLEARANCE_TUCK).conflict;
    const bool noteStillCollides = requiredVerticalMoveFromNotationRect(finalRect, ctx.noteDangerRect, ctx.above,
                                                                        PM_FINGERING_NOTE_CLEARANCE_MIN * spatium) > 0.0;
    if (notationStillCollides || noteStillCollides) {
        ++MScore::pianomaniaManualReviewFingerings;
        LOGW() << "Pianomania fingering/notation clearance needs manual review at tick " << chord->tick().ticks()
               << ", track " << chord->track();
    }

    if (adjustment) {
        adjustment->fingerings = fingerings;
        adjustment->chord = chord;
        adjustment->segment = chord->segment();
        adjustment->ctx = ctx;
        adjustment->baseRect = rect;
        adjustment->chosen = chosen;
        adjustment->allowTuck = allowTuck;
        adjustment->flipped = flipped;
        adjustment->valid = true;
    }

    return true;
}

double fingeringGroupBaseline(const RectF& rect, bool above)
{
    return above ? rect.bottom() : rect.top();
}

void collectNonGraceFingeringBaselinesForSegment(const Segment* segment, staff_idx_t staffIdx, bool above,
                                                 std::vector<double>& baselines)
{
    if (!segment || !segment->isChordRestType()) {
        return;
    }

    std::vector<Fingering*> segmentFingerings;
    for (EngravingItem* item : segment->elist()) {
        if (!item || !item->isChord()) {
            continue;
        }
        Chord* chord = toChord(item);
        if (chord->isGrace() || chord->vStaffIdx() != staffIdx) {
            continue;
        }
        for (Note* note : chord->notes()) {
            for (EngravingItem* noteItem : note->el()) {
                if (!noteItem->isFingering()) {
                    continue;
                }
                Fingering* fingering = toFingering(noteItem);
                if ((fingering->placement() == PlacementV::ABOVE) == above) {
                    segmentFingerings.push_back(fingering);
                }
            }
        }
    }

    const RectF rect = fingeringGroupSystemRect(segmentFingerings);
    if (!rect.isNull()) {
        baselines.push_back(fingeringGroupBaseline(rect, above));
    }
}

bool rectClearsGraceFingeringAlignment(const RectF& baseRect, const RectF& rect, const std::vector<Fingering*>& fingerings,
                                       const std::vector<FingeringObstacle>& obstacles, staff_idx_t staffIdx,
                                       const System* system, double staffTop, bool above, double spatium)
{
    const RectF danger = fingeringGroupNotationDangerRect(fingerings, 0.2 * spatium, false);
    if (requiredVerticalMoveFromNotationRect(rect, danger, above, PM_FINGERING_NOTE_CLEARANCE_MIN * spatium) > 0.0
        || requiredVerticalMoveFromMarkObstacles(rect, obstacles, staffIdx, above,
                                                 PM_FINGERING_MARK_CLEARANCE_MIN * spatium) > 0.0
        || slurAvoidanceForRect(rect, obstacles, staffIdx, staffTop, above, spatium,
                                PM_FINGERING_SLUR_CLEARANCE_TUCK).conflict) {
        return false;
    }

    // Alignment may hop the digit across a slur into the pocket its neighbors
    // occupy, so the landing spot must also be proven against the skyline
    // (stems, beams, accidentals the obstacle model doesn't carry).
    FingeringGroupContext ctx;
    ctx.system = system;
    ctx.groupShape = fingeringGroupSystemShape(fingerings);
    ctx.staffIdx = staffIdx;
    ctx.staffTop = staffTop;
    ctx.above = above;
    ctx.spatium = spatium;
    return fingeringGroupFinalTuckClearsSkyline(baseRect, rect, ctx);
}

bool alignGraceFingeringGroupToNearbyMainBaseline(const std::vector<Fingering*>& fingerings,
                                                  const std::vector<FingeringObstacle>& obstacles)
{
    if (fingerings.empty()) {
        return false;
    }

    Fingering* first = fingerings.front();
    const Note* note = first->note();
    Chord* chord = note ? note->chord() : nullptr;
    if (!chord || !chord->isGrace() || !chord->explicitParent() || !chord->explicitParent()->isChord()) {
        return false;
    }

    Chord* host = toChord(chord->explicitParent());
    const Segment* hostSegment = host->segment();
    if (!hostSegment) {
        return false;
    }

    const staff_idx_t staffIdx = chord->vStaffIdx();
    const bool above = first->placement() == PlacementV::ABOVE;
    const double spatium = first->spatium();
    std::vector<double> candidateBaselines;
    collectNonGraceFingeringBaselinesForSegment(hostSegment, staffIdx, above, candidateBaselines);
    collectNonGraceFingeringBaselinesForSegment(hostSegment->next1(SegmentType::ChordRest), staffIdx, above,
                                                candidateBaselines);
    if (candidateBaselines.empty()) {
        return false;
    }

    const RectF rect = fingeringGroupSystemRect(fingerings);
    if (rect.isNull()) {
        return false;
    }

    const double baseline = fingeringGroupBaseline(rect, above);
    // The neighbor's baseline is proof its spot is viable, so the hop may be
    // large — including across a slur into the pocket the neighbors tucked
    // into (a grace digit left outside the slur while its row sits within
    // reads as detached). Every landing is clearance- and skyline-verified.
    const double maxGap = 3.6 * spatium;
    const Measure* measure = hostSegment->measure();
    const System* system = measure ? measure->system() : nullptr;
    const double staffTop = staffYInSystem(system, staffIdx);

    std::sort(candidateBaselines.begin(), candidateBaselines.end(), [baseline](double a, double b) {
        return std::abs(a - baseline) < std::abs(b - baseline);
    });
    // A grace's own stem or flag often occupies the pocket directly over its
    // notehead, so the digit may also dodge sideways a little — beside the
    // takeoff is how engraved sources treat grace digits.
    const std::array<double, 3> dxCandidates = { 0.0, -0.5 * spatium, 0.5 * spatium };
    for (double candidate : candidateBaselines) {
        const double gap = std::abs(candidate - baseline);
        if (gap > maxGap || muse::RealIsNull(gap)) {
            continue;
        }
        // The exact row baseline may graze the beam at the grace's own x (the
        // beam slopes; the row was placed against ITS local beam surface), so
        // scan a small away-from-staff correction: slightly above the row but
        // still under the slur reads as part of the group.
        const double liftStep = 0.1 * spatium;
        const int maxLiftSteps = 10;
        for (double dx : dxCandidates) {
            for (int lift = 0; lift <= maxLiftSteps; ++lift) {
                const double liftOffset = (above ? -1.0 : 1.0) * lift * liftStep;
                const RectF moved = rect.translated(dx, candidate - baseline + liftOffset);
                if (!rectClearsGraceFingeringAlignment(rect, moved, fingerings, obstacles, staffIdx, system, staffTop,
                                                       above, spatium)) {
                    continue;
                }
                for (Fingering* fingering : fingerings) {
                    fingering->mutldata()->moveX(dx);
                    fingering->mutldata()->moveY(candidate - baseline + liftOffset);
                }
                return true;
            }
        }
    }
    return false;
}

// A run of digits over a monotonic note line must follow the line's
// direction: a digit sitting level with (or against) its predecessor over a
// rising run reads as a typo even when each digit is fine in isolation.
// Chains are consecutive same-staff/side fingering groups whose chords share
// a beam or a covering slur; short pairs are left alone so deliberately level
// placements are not disturbed. Offenders are only ever moved away from the
// staff (the safe direction) and re-verified against marks and slurs.
struct PmFingeringChainNode {
    std::vector<Fingering*> fingerings;
    Chord* chord = nullptr;
    Segment* segment = nullptr;
    double noteY = 0.0;
    RectF noteRect;
    RectF rect;
};

const Slur* pianomaniaSharedCoveringSlur(const Chord* a, const Chord* b, staff_idx_t staffIdx,
                                         const std::vector<FingeringObstacle>& obstacles)
{
    if (!a || !b) {
        return nullptr;
    }

    const Chord* first = a;
    const Chord* second = b;
    if (second->tick() < first->tick()) {
        std::swap(first, second);
    }

    // Only temporally adjacent chords chain: a long phrase slur covers distant
    // digits whose heights are set by unrelated local geometry (takeoffs,
    // whole-note anchors) and must not be dragged to a common contour.
    if (second->tick() - first->tick() > Fraction(1, 4)) {
        return nullptr;
    }

    const Fraction firstTick = first->tick();
    const Fraction secondTick = second->tick();
    for (const FingeringObstacle& obstacle : obstacles) {
        if (!obstacle.slurSegment || obstacle.staffIdx != staffIdx) {
            continue;
        }
        const Slur* slur = obstacle.slurSegment->slur();
        if (slur && slur->tick() <= firstTick && slur->tick2() >= secondTick) {
            return slur;
        }
    }
    return nullptr;
}

bool pianomaniaChordsShareBeamOrSlur(const Chord* a, const Chord* b, staff_idx_t staffIdx,
                                     const std::vector<FingeringObstacle>& obstacles)
{
    // Only temporally adjacent chords chain: a long phrase slur covers distant
    // digits whose heights are set by unrelated local geometry (takeoffs,
    // whole-note anchors) and must not be dragged to a common contour.
    if (b->tick() - a->tick() > Fraction(1, 4)) {
        return false;
    }

    if (a->beam() && a->beam() == b->beam()) {
        return true;
    }

    return pianomaniaSharedCoveringSlur(a, b, staffIdx, obstacles);
}

void applyFingeringGroupPlacementDelta(PmFingeringGroupAdjustment& adjustment, const GroupPlacement& placement)
{
    const double currentDy = adjustment.ctx.above ? -adjustment.chosen.moveAway : adjustment.chosen.moveAway;
    const double targetDy = adjustment.ctx.above ? -placement.moveAway : placement.moveAway;
    const double dx = placement.dx - adjustment.chosen.dx;
    const double dy = targetDy - currentDy;
    if (muse::RealIsNull(dx) && muse::RealIsNull(dy)) {
        adjustment.chosen = placement;
        return;
    }

    for (Fingering* fingering : adjustment.fingerings) {
        fingering->mutldata()->moveX(dx);
        fingering->mutldata()->moveY(dy);
    }

    adjustment.chosen = placement;
    if (adjustment.segment && adjustment.ctx.staffIdx != muse::nidx) {
        adjustment.segment->createShape(adjustment.ctx.staffIdx);
    }
}

void retryPianomaniaSlurChainTucks(std::vector<PmFingeringGroupAdjustment>& adjustments,
                                   const std::vector<size_t>& chain)
{
    double neighborAllowance = 0.0;
    bool hasTucked = false;
    bool hasUntucked = false;
    for (const size_t index : chain) {
        const PmFingeringGroupAdjustment& adjustment = adjustments[index];
        hasTucked = hasTucked || adjustment.chosen.tucked;
        hasUntucked = hasUntucked || !adjustment.chosen.tucked;
        if (adjustment.chosen.tucked) {
            neighborAllowance = std::max(neighborAllowance, adjustment.chosen.tuckAllowanceProven);
        }
    }

    if (!hasTucked || !hasUntucked || neighborAllowance <= 0.0) {
        return;
    }

    std::vector<std::pair<size_t, GroupPlacement>> retries;
    for (const size_t index : chain) {
        const PmFingeringGroupAdjustment& adjustment = adjustments[index];
        if (adjustment.chosen.tucked) {
            continue;
        }
        // moveAway is slur-driven displacement only: a member the slur never
        // pushed (marks or staff clearance placed it) has nothing to retry and
        // must not veto fixes for the chain's genuine failed tucks.
        if (adjustment.chosen.moveAway <= 0.0) {
            continue;
        }

        FingeringGroupContext retryCtx = adjustment.ctx;
        retryCtx.neighborTuckAllowance = neighborAllowance;

        // Retry only at the group's chosen dx: a dodge dx (e.g. left of an
        // sf-dynamic's reserved zone) encodes a constraint the resolver cannot
        // see, so re-centering behind its back could land in the reserved spot.
        GroupPlacement retry = resolveFingeringGroupPlacement(adjustment.baseRect, adjustment.chosen.dx, retryCtx);

        const double minimumImprovement = 0.05 * retryCtx.spatium;
        if (!retry.tucked || retry.moveAway > adjustment.chosen.moveAway - minimumImprovement) {
            return;
        }
        retries.emplace_back(index, retry);
    }

    for (const auto& [index, placement] : retries) {
        applyFingeringGroupPlacementDelta(adjustments[index], placement);
    }
}

void enforcePianomaniaSlurTuckCoherence(std::vector<PmFingeringGroupAdjustment>& adjustments,
                                        const std::vector<FingeringObstacle>& obstacles)
{
    if (adjustments.size() < 2) {
        return;
    }

    std::set<staff_idx_t> staves;
    for (const PmFingeringGroupAdjustment& adjustment : adjustments) {
        if (adjustment.valid && adjustment.allowTuck && adjustment.chord
            && adjustment.ctx.staffIdx != muse::nidx) {
            staves.insert(adjustment.ctx.staffIdx);
        }
    }

    for (staff_idx_t staffIdx : staves) {
        for (int side = 0; side < 2; ++side) {
            const bool above = side == 1;
            std::vector<size_t> ordered;
            ordered.reserve(adjustments.size());
            for (size_t i = 0; i < adjustments.size(); ++i) {
                const PmFingeringGroupAdjustment& adjustment = adjustments[i];
                if (adjustment.valid && adjustment.allowTuck && adjustment.chord
                    && adjustment.ctx.staffIdx == staffIdx && adjustment.ctx.above == above) {
                    ordered.push_back(i);
                }
            }

            std::stable_sort(ordered.begin(), ordered.end(), [&adjustments](size_t a, size_t b) {
                return adjustments[a].chord->tick() < adjustments[b].chord->tick();
            });

            size_t start = 0;
            while (start < ordered.size()) {
                std::vector<size_t> chain = { ordered[start] };
                const Slur* chainSlur = nullptr;
                size_t end = start;
                while (end + 1 < ordered.size()) {
                    const PmFingeringGroupAdjustment& current = adjustments[ordered[end]];
                    const PmFingeringGroupAdjustment& next = adjustments[ordered[end + 1]];
                    const Slur* sharedSlur = pianomaniaSharedCoveringSlur(current.chord, next.chord, staffIdx, obstacles);
                    if (!sharedSlur || (chainSlur && chainSlur != sharedSlur)) {
                        break;
                    }
                    chainSlur = sharedSlur;
                    chain.push_back(ordered[end + 1]);
                    ++end;
                }

                if (chainSlur && chain.size() >= 2) {
                    retryPianomaniaSlurChainTucks(adjustments, chain);
                }

                start = end + 1;
            }
        }
    }
}

void enforcePianomaniaFingeringRunCoherence(System* system, const std::vector<FingeringObstacle>& obstacles)
{
    constexpr size_t minChainLength = 3;

    for (int side = 0; side < 2; ++side) {
        const bool above = side == 1;

        std::vector<PmFingeringChainNode> nodes;
        for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure()) {
                continue;
            }
            for (Segment& segment : toMeasure(mb)->segments()) {
                if (!segment.isChordRestType()) {
                    continue;
                }
                for (EngravingItem* item : segment.elist()) {
                    if (!item || !item->isChord()) {
                        continue;
                    }
                    Chord* chord = toChord(item);
                    PmFingeringChainNode node;
                    bool manual = false;
                    for (Note* note : chord->notes()) {
                        for (EngravingItem* noteItem : note->el()) {
                            if (!noteItem->isFingering()) {
                                continue;
                            }
                            Fingering* fingering = toFingering(noteItem);
                            if ((fingering->placement() == PlacementV::ABOVE) != above) {
                                continue;
                            }
                            manual = manual || hasManualFingeringPlacement(fingering);
                            node.fingerings.push_back(fingering);
                        }
                    }
                    if (node.fingerings.empty() || (manual && !MScore::pianomaniaForceNormalizeSlursFingerings)) {
                        continue;
                    }
                    node.chord = chord;
                    node.segment = &segment;
                    const Note* lineNote = above ? chord->upNote() : chord->downNote();
                    const RectF noteRect = noteSystemRect(lineNote);
                    node.rect = fingeringGroupSystemRect(node.fingerings);
                    if (noteRect.isNull() || node.rect.isNull()) {
                        continue;
                    }
                    node.noteY = noteRect.center().y();
                    node.noteRect = noteRect;
                    nodes.push_back(std::move(node));
                }
            }
        }

        std::set<staff_idx_t> staves;
        for (const PmFingeringChainNode& node : nodes) {
            staves.insert(node.chord->vStaffIdx());
        }

        for (staff_idx_t staffIdx : staves) {
            std::vector<PmFingeringChainNode*> staffNodes;
            for (PmFingeringChainNode& node : nodes) {
                if (node.chord->vStaffIdx() == staffIdx) {
                    staffNodes.push_back(&node);
                }
            }

            const double staffTop = staffYInSystem(system, staffIdx);
            size_t start = 0;
            while (start < staffNodes.size()) {
                size_t end = start;
                while (end + 1 < staffNodes.size()
                       && pianomaniaChordsShareBeamOrSlur(staffNodes[end]->chord, staffNodes[end + 1]->chord,
                                                          staffIdx, obstacles)) {
                    ++end;
                }

                const size_t length = end - start + 1;
                if (length >= minChainLength) {
                    const double sp = staffNodes[start]->fingerings.front()->spatium();
                    // The run must be strictly monotonic toward the digit side
                    // (rising under above-digits, falling under below-digits).
                    bool monotonic = true;
                    for (size_t i = start; i < end && monotonic; ++i) {
                        const double delta = staffNodes[i + 1]->noteY - staffNodes[i]->noteY;
                        monotonic = above ? delta < -0.25 * sp : delta > 0.25 * sp;
                    }

                    if (monotonic) {
                        double prevBaseline = above ? staffNodes[start]->rect.bottom() : staffNodes[start]->rect.top();
                        for (size_t i = start + 1; i <= end; ++i) {
                            PmFingeringChainNode& node = *staffNodes[i];
                            const double noteDelta = std::abs(node.noteY - staffNodes[i - 1]->noteY);
                            const double minStep = std::min(noteDelta, 0.5 * sp);
                            const double baseline = above ? node.rect.bottom() : node.rect.top();
                            const double target = above ? prevBaseline - minStep : prevBaseline + minStep;
                            const double shortfall = above ? baseline - target : target - baseline;
                            if (shortfall > 0.1 * sp) {
                                double away = shortfall;
                                RectF moved = node.rect.translated(0.0, above ? -away : away);
                                // Moving away can run into marks or another
                                // slur; keep moving away until clear.
                                for (int guard = 0; guard < 3; ++guard) {
                                    const double extra = std::max(
                                        requiredVerticalMoveFromMarkObstacles(moved, obstacles, staffIdx, above,
                                                                              PM_FINGERING_MARK_CLEARANCE_MIN * sp),
                                        slurAvoidanceForRect(moved, obstacles, staffIdx, staffTop, above, sp).awayMove);
                                    if (extra <= 0.0) {
                                        break;
                                    }
                                    away += extra;
                                    moved.translate(0.0, above ? -extra : extra);
                                }
                                // Slur context beats strict contour stepping: when the
                                // avoidance loop inflates a small contour correction into a
                                // leap past the covering slur, a level digit that stays in
                                // its pocket reads better than one lofted above the phrase.
                                if (away > shortfall + 0.75 * sp) {
                                    prevBaseline = above ? node.rect.bottom() : node.rect.top();
                                    continue;
                                }
                                if (fingeringRectDistanceFromNoteheads(moved, node.noteRect, above)
                                    > PM_FINGERING_NOTEHEAD_DETACHMENT_CAP * sp) {
                                    prevBaseline = above ? node.rect.bottom() : node.rect.top();
                                    continue;
                                }
                                for (Fingering* fingering : node.fingerings) {
                                    fingering->mutldata()->moveY(above ? -away : away);
                                }
                                node.rect = moved;
                                node.segment->createShape(staffIdx);
                            }
                            prevBaseline = above ? node.rect.bottom() : node.rect.top();
                        }
                    }
                }

                start = end + 1;
            }
        }
    }
}

void adjustPianomaniaFingeringsAroundNotationForSystem(System* system, bool addFinalRectsToSkylines)
{
    const std::vector<FingeringObstacle> obstacles = collectPianomaniaFingeringObstacles(system);
    std::vector<PmFingeringGroupAdjustment> adjustments;

    // Digits of one segment column that resolve to the same displayed staff
    // and side must be treated as ONE group even when they come from chords
    // of different voices: vanilla autoplace resolves each track against a
    // stale skyline and prints them on top of each other. Grace chords keep
    // their own groups — they anchor to their own x position.
    auto processGroup = [&obstacles, &adjustments](const std::vector<Fingering*>& group, bool multiChord, bool graceGroup,
                                                   bool allowOppositeSide) -> bool {
        if (group.empty()) {
            return false;
        }
        bool manual = false;
        for (const Fingering* fingering : group) {
            manual = manual || hasManualFingeringPlacement(fingering);
        }
        // Stale-manual rescue is limited to GRACE groups: a grace digit's baked
        // source offset was authored against the original engraving and lands on
        // the neighboring beamed group once auto-layout recasts the systems. For
        // normal groups a manual placement stays source authority even when it
        // grazes notation — rescuing those en masse inflates skylines on
        // manual-heavy scores (chopin op47/3) and recasts their pages.
        const bool rescueManualPlacement = manual && !MScore::pianomaniaForceNormalizeSlursFingerings
                                           && ((graceGroup && manualFingeringGroupOverlapsNotation(group, graceGroup))
                                               || fingeringGroupPreferredSideTupletBlocked(group, obstacles));
        if (manual && !MScore::pianomaniaForceNormalizeSlursFingerings && !rescueManualPlacement) {
            // Manual placement is source authority; re-centering it
            // would also undo the horizontal nudges persisted by a
            // previous Prettify run.
            return false;
        }
        bool moved = false;
        if (multiChord) {
            // Cross-voice digits meeting on one side stack like the digits of
            // a single chord (outermost note's digit outermost).
            setFingeringGroupPlacementAndRelayout(group, group.front()->placement());
            moved = true;
        }
        moved = centerFingeringGroupOverNotes(group) || moved;
        PmFingeringGroupAdjustment adjustment;
        const bool adjusted = adjustFingeringGroupAroundNotation(group, obstacles, allowOppositeSide, !graceGroup,
                                                                 0.0, &adjustment, rescueManualPlacement);
        if (adjustment.valid) {
            adjustments.push_back(std::move(adjustment));
        }
        if (graceGroup) {
            moved = alignGraceFingeringGroupToNearbyMainBaseline(group, obstacles) || moved;
        }
        moved = adjusted || moved;
        return moved;
    };

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* measure = toMeasure(mb);
        for (Segment& segment : measure->segments()) {
            if (!segment.isChordRestType()) {
                continue;
            }

            std::map<staff_idx_t, std::array<std::vector<Fingering*>, 2>> mainGroups;
            std::map<staff_idx_t, std::array<int, 2>> mainChordCounts;
            struct GraceChordGroups {
                Chord* chord = nullptr;
                std::array<std::vector<Fingering*>, 2> groups;
            };
            std::vector<GraceChordGroups> graceGroups;

            auto collectChordFingerings = [](Chord* chord, std::array<std::vector<Fingering*>, 2>& groups) {
                std::array<bool, 2> contributed = { false, false };
                for (Note* note : chord->notes()) {
                    for (EngravingItem* noteItem : note->el()) {
                        if (!noteItem->isFingering()) {
                            continue;
                        }
                        Fingering* fingering = toFingering(noteItem);
                        normalizeManualFingeringIfRequested(fingering);
                        applyPianomaniaPreferredFingeringPlacement(fingering);
                        const bool preferAbove = pianomaniaPreferredFingeringPlacement(fingering) == PlacementV::ABOVE;
                        groups[preferAbove ? 1 : 0].push_back(fingering);
                        contributed[preferAbove ? 1 : 0] = true;
                    }
                }
                return contributed;
            };

            for (EngravingItem* item : segment.elist()) {
                if (!item || !item->isChord()) {
                    continue;
                }

                Chord* chord = toChord(item);
                const std::array<bool, 2> contributed = collectChordFingerings(chord, mainGroups[chord->vStaffIdx()]);
                for (size_t side = 0; side < contributed.size(); ++side) {
                    mainChordCounts[chord->vStaffIdx()][side] += contributed[side] ? 1 : 0;
                }
                for (Chord* grace : chord->graceNotes()) {
                    GraceChordGroups graceEntry;
                    graceEntry.chord = grace;
                    collectChordFingerings(grace, graceEntry.groups);
                    if (!graceEntry.groups[0].empty() || !graceEntry.groups[1].empty()) {
                        graceGroups.push_back(std::move(graceEntry));
                    }
                }
            }

            std::set<staff_idx_t> movedStaves;
            for (auto& staffGroups : mainGroups) {
                std::array<std::vector<Fingering*>, 2>& groups = staffGroups.second;
                for (size_t groupIdx = 0; groupIdx < groups.size(); ++groupIdx) {
                    // Flipping to the other side is only safe when that side
                    // isn't occupied by this column's other fingering group.
                    const bool allowOppositeSide = groups[1 - groupIdx].empty();
                    const bool multiChord = mainChordCounts[staffGroups.first][groupIdx] > 1;
                    if (processGroup(groups[groupIdx], multiChord, false, allowOppositeSide)) {
                        movedStaves.insert(staffGroups.first);
                    }
                }
            }
            for (GraceChordGroups& graceEntry : graceGroups) {
                for (size_t groupIdx = 0; groupIdx < graceEntry.groups.size(); ++groupIdx) {
                    const bool allowOppositeSide = graceEntry.groups[1 - groupIdx].empty();
                    if (processGroup(graceEntry.groups[groupIdx], false, true, allowOppositeSide)) {
                        movedStaves.insert(graceEntry.chord->vStaffIdx());
                    }
                }
            }
            for (staff_idx_t staffIdx : movedStaves) {
                segment.createShape(staffIdx);
            }
        }
    }

    enforcePianomaniaSlurTuckCoherence(adjustments, obstacles);
    enforcePianomaniaFingeringRunCoherence(system, obstacles);

    // The skylines were built before this pass, so they hold every digit at
    // its vanilla position. Publishing the final rects lets everything laid
    // out afterwards — dynamics, hairpins, and above all the inter-staff
    // distances of layout2 — reserve room for where the digits actually are
    // (e.g. a treble below-digit and a bass above-digit sharing a tight
    // inter-staff gap). The items are tagged so the fingering filters of the
    // page-stage rerun still exclude them. The replay layout (prettify flags
    // off) needs no equivalent: persisted offsets flow into the segment
    // shapes before the skylines are built.
    if (!addFinalRectsToSkylines) {
        return;
    }
    // The stale pre-pass rects are dropped, not just overlaid: a skyline is a
    // max-envelope, so a digit whose vanilla autoplace lofted it (e.g. above a
    // cross-staff beam) would otherwise keep claiming that space from layout2
    // even after this pass tucked it beside its notehead. Which position the
    // pre-pass layout produces depends on replayed offset state, so leaving
    // the stale rects in makes staff distances differ between the transient
    // pipeline and the persisted replay of the very same digit positions.
    auto dropFingeringRects = [](SkylineLine& line) {
        std::vector<ShapeElement>& elements = line.elements();
        elements.erase(std::remove_if(elements.begin(), elements.end(), [](const ShapeElement& el) {
            return el.item() && el.item()->isFingering();
        }), elements.end());
    };
    for (SysStaff* sysStaff : system->staves()) {
        dropFingeringRects(sysStaff->skyline().north());
        dropFingeringRects(sysStaff->skyline().south());
    }
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (Segment& segment : toMeasure(mb)->segments()) {
            if (!segment.isChordRestType()) {
                continue;
            }
            for (EngravingItem* item : segment.elist()) {
                if (!item || !item->isChord()) {
                    continue;
                }
                Chord* chord = toChord(item);
                std::vector<Chord*> chords { chord };
                for (Chord* grace : chord->graceNotes()) {
                    chords.push_back(grace);
                }
                for (Chord* c : chords) {
                    for (Note* note : c->notes()) {
                        for (EngravingItem* noteItem : note->el()) {
                            if (!noteItem->isFingering() || !noteItem->addToSkyline()) {
                                continue;
                            }
                            Fingering* fingering = toFingering(noteItem);
                            const RectF rect = fingeringSystemRect(fingering);
                            if (rect.isNull()) {
                                continue;
                            }
                            system->staff(c->vStaffIdx())->skyline().add(rect, fingering);
                        }
                    }
                }
            }
        }
    }
}

// Hairpins autoplace against the staff skyline, which cannot guarantee
// clearance against a slur's arc at every x (the m9/m11 Arietta pattern:
// the arc slips through the open aperture at a cresc/dim junction). Engraved
// practice puts the hairpin outside the slur, so every snapping chain that
// contains a hairpin is moved away from the staff until its glyph boxes clear
// the slur curves under their spans. This is transient layout geometry;
// Prettify persists the shift as segment offsets so the flags-off replay
// reproduces it (autoplace only ever pushes spanner segments away from the
// staff, so the persisted offset replays linearly).
void liftPianomaniaHairpinChainsClearOfSlurs(System* system, const std::vector<EngravingItem*>& alignedItems)
{
    if (!system || alignedItems.empty()) {
        return;
    }

    const std::set<EngravingItem*> pool(alignedItems.begin(), alignedItems.end());
    std::set<EngravingItem*> visited;

    for (EngravingItem* item : alignedItems) {
        if (!item || !item->isHairpinSegment() || visited.count(item)) {
            continue;
        }

        // Gather the snapping chain this hairpin belongs to.
        std::vector<EngravingItem*> chain;
        EngravingItem* start = item;
        while (true) {
            EngravingItem* prev = start->ldata() ? start->ldata()->itemSnappedBefore() : nullptr;
            if (!prev || !pool.count(prev) || visited.count(prev)) {
                break;
            }
            start = prev;
        }
        for (EngravingItem* link = start; link;) {
            chain.push_back(link);
            visited.insert(link);
            EngravingItem* next = link->ldata() ? link->ldata()->itemSnappedAfter() : nullptr;
            link = (next && pool.count(next) && !visited.count(next)) ? next : nullptr;
        }

        double lift = 0.0;
        bool above = true;
        const double spatium = item->spatium();
        const double clearance = PM_FINGERING_SLUR_CLEARANCE * spatium;
        for (EngravingItem* member : chain) {
            if (!member->isHairpinSegment()) {
                continue;
            }
            const SpannerSegment* hairpinSegment = toSpannerSegment(member);
            above = hairpinSegment->spanner()->placeAbove();
            const staff_idx_t staffIdx = hairpinSegment->vStaffIdx();
            const RectF rect = hairpinSegment->ldata()->bbox().translated(
                hairpinSegment->pos() + PointF(0.0, staffYInSystem(system, staffIdx)));
            for (SpannerSegment* spannerSegment : system->spannerSegments()) {
                if (!spannerSegment->isSlurSegment()) {
                    continue;
                }
                const SlurSegment* slurSegment = toSlurSegment(spannerSegment);
                if (slurSegment->vStaffIdx() != staffIdx) {
                    continue;
                }
                const SlurWindowSample sample = sampleSlurWithinFingeringWindow(rect, slurSegment, 0.0, 0.0);
                if (!sample.any) {
                    continue;
                }
                if (above) {
                    lift = std::max(lift, rect.bottom() + clearance - sample.minY);
                } else {
                    lift = std::max(lift, sample.maxY + clearance - rect.top());
                }
            }
        }

        if (lift <= 0.05 * spatium) {
            continue;
        }
        for (EngravingItem* member : chain) {
            member->mutldata()->moveY(above ? -lift : lift);
        }
    }
}

void movePianomaniaTextHairpinsClearOfNotation(System* system)
{
    if (!system) {
        return;
    }

    std::map<staff_idx_t, std::vector<TextHairpinObstacle>> obstaclesByStaff;
    for (SpannerSegment* segment : system->spannerSegments()) {
        if (!segment || !segment->isHairpinSegment() || !segment->visible()
            || !segment->ldata() || segment->ldata()->isSkipDraw()) {
            continue;
        }

        HairpinSegment* hairpinSegment = toHairpinSegment(segment);
        Hairpin* hairpin = hairpinSegment->hairpin();
        if (!hairpin || !hairpin->isLineType()) {
            continue;
        }

        const staff_idx_t staffIdx = hairpinSegment->vStaffIdx();
        const double spatium = std::max(1.0, hairpinSegment->spatium());
        RectF rect = hairpinSegmentSystemRect(hairpinSegment);
        if (rect.isNull()) {
            continue;
        }

        auto& obstacles = obstaclesByStaff[staffIdx];
        if (obstacles.empty()) {
            obstacles = collectPianomaniaTextHairpinObstacles(system, staffIdx);
        }

        const double clearance = PM_TEXT_HAIRPIN_NOTATION_CLEARANCE * spatium;
        bool collides = false;
        double moveAway = 0.0;
        const bool above = hairpinSegment->spanner()->placeAbove();
        for (const TextHairpinObstacle& obstacleEntry : obstacles) {
            const RectF& obstacle = obstacleEntry.rect;
            if (obstacle.isNull()
                || rect.left() >= obstacle.right() || rect.right() <= obstacle.left()) {
                continue;
            }

            const double xOverlap = std::min(rect.right(), obstacle.right()) - std::max(rect.left(), obstacle.left());
            const double yOverlap = std::min(rect.bottom(), obstacle.bottom()) - std::max(rect.top(), obstacle.top());
            const bool genuineOverlap = xOverlap > PM_TEXT_HAIRPIN_MIN_OVERLAP * spatium
                                        && yOverlap > PM_TEXT_HAIRPIN_MIN_OVERLAP * spatium;
            if (above) {
                if (genuineOverlap) {
                    collides = true;
                }
                if (obstacle.top() < rect.bottom()) {
                    moveAway = std::max(moveAway, rect.bottom() + clearance - obstacle.top());
                }
            } else {
                if (genuineOverlap) {
                    collides = true;
                }
                if (obstacle.bottom() > rect.top()) {
                    moveAway = std::max(moveAway, obstacle.bottom() + clearance - rect.top());
                }
            }
        }

        if (!collides || moveAway <= 0.05 * spatium) {
            continue;
        }

        hairpinSegment->mutldata()->moveY(above ? -moveAway : moveAway);
    }
}
}

void SystemLayout::adjustPianomaniaFingeringsAroundNotation(System* system, bool addFinalRectsToSkylines)
{
    if (!system || !MScore::pianomaniaPrettifySlursFingerings) {
        return;
    }

    adjustPianomaniaFingeringsAroundNotationForSystem(system, addFinalRectsToSkylines);
}

//---------------------------------------------------------
//   collectSystem
//---------------------------------------------------------

System* SystemLayout::collectSystem(LayoutContext& ctx)
{
    TRACEFUNC;

    if (!ctx.state().curMeasure()) {
        return nullptr;
    }

    const MeasureBase* measure = ctx.dom().systems().empty() ? 0 : ctx.dom().systems().back()->measures().back();
    if (measure) {
        measure = measure->mbWithPrecedingSectionBreak();
    }

    bool firstSysLongName = ctx.conf().styleV(Sid::firstSystemInstNameVisibility).value<InstrumentLabelVisibility>()
                            == InstrumentLabelVisibility::LONG;
    bool subsSysLongName = ctx.conf().styleV(Sid::subsSystemInstNameVisibility).value<InstrumentLabelVisibility>()
                           == InstrumentLabelVisibility::LONG;
    if (measure) {
        ctx.mutState().setFirstSystem(measure->sectionBreak() && !ctx.conf().isFloatMode());
        if (const LayoutBreak* layoutBreak = measure->sectionBreakElement()) {
            ctx.mutState().setFirstSystemIndent(ctx.state().firstSystem()
                                                && ctx.conf().firstSystemIndent()
                                                && layoutBreak->firstSystemIndentation());
            ctx.mutState().setStartWithLongNames(
                ctx.state().firstSystem() && firstSysLongName && layoutBreak->startWithLongNames());
        }
    } else {
        ctx.mutState().setStartWithLongNames(ctx.state().firstSystem() && firstSysLongName);
    }

    System* system = getNextSystem(ctx);
    for (SysStaff* staff : system->staves()) {
        staff->skyline().clear();
    }

    LAYOUT_CALL() << LAYOUT_ITEM_INFO(system);

    Fraction lcmTick = ctx.state().curMeasure()->tick();
    bool longNames = ctx.mutState().firstSystem() ? ctx.mutState().startWithLongNames() : subsSysLongName;
    SystemLayout::setInstrumentNames(system, ctx, longNames, lcmTick);

    double curSysWidth = 0.0;
    double layoutSystemMinWidth = 0.0;
    double targetSystemWidth = ctx.conf().styleD(Sid::pagePrintableWidth) * DPI;
    system->setWidth(targetSystemWidth);

    // save state of measure
    MeasureBase* breakMeasure = nullptr;

    System* oldSystem = nullptr;

    MeasureState prevMeasureState;
    prevMeasureState.curHeader = ctx.state().curMeasure()->header();
    prevMeasureState.curTrailer = ctx.state().curMeasure()->trailer();

    const SystemLock* systemLock = ctx.conf().viewMode() == LayoutMode::PAGE || ctx.conf().viewMode() == LayoutMode::SYSTEM
                                   ? ctx.dom().systemLocks()->lockStartingAt(ctx.state().curMeasure()) : nullptr;

    while (ctx.state().curMeasure()) {      // collect measure for system
        oldSystem = ctx.mutState().curMeasure()->system();
        system->appendMeasure(ctx.mutState().curMeasure());

        if (ctx.state().curMeasure()->isMeasure()) {
            Measure* m = toMeasure(ctx.mutState().curMeasure());
            if (!(oldSystem && oldSystem->page() && oldSystem->page() != ctx.state().page())) {
                MeasureLayout::computePreSpacingItems(m, ctx);
            }

            if (measureHasCrossStuffOrModifiedBeams(m)) {
                updateCrossBeams(system, ctx);
            }

            if (m->isFirstInSystem()) {
                layoutSystemMinWidth = curSysWidth;
                SystemLayout::layoutSystem(system, ctx, curSysWidth, ctx.state().firstSystem(), ctx.state().firstSystemIndent());
                MeasureLayout::addSystemHeader(m, ctx.state().firstSystem(), ctx);
            } else {
                bool createHeader = ctx.state().prevMeasure()->isHBox() && toHBox(ctx.state().prevMeasure())->createSystemHeader();
                if (createHeader) {
                    MeasureLayout::addSystemHeader(m, false, ctx);
                } else if (m->header()) {
                    MeasureLayout::removeSystemHeader(m);
                }
            }

            MeasureLayout::createEndBarLines(m, true, ctx);

            if (m->noBreak() || systemLock) {
                MeasureLayout::removeSystemTrailer(m, ctx);
            } else {
                MeasureLayout::addSystemTrailer(m, m->nextMeasure(), ctx);
            }

            MeasureLayout::setRepeatCourtesiesAndParens(m, ctx);

            MeasureLayout::updateGraceNotes(m, ctx);

            if (!systemLock) {
                curSysWidth = HorizontalSpacing::updateSpacingForLastAddedMeasure(system);
            }
        } else if (ctx.state().curMeasure()->isHBox()) {
            if (!systemLock) {
                curSysWidth = HorizontalSpacing::updateSpacingForLastAddedMeasure(system);
            }
        } else {
            // vbox:
            MeasureLayout::getNextMeasure(ctx);
            SystemLayout::layout2(system, ctx);         // compute staff distances
            return system;
        }

        bool doBreak = !systemLock && system->measures().size() > 1 && curSysWidth > targetSystemWidth
                       && !ctx.state().prevMeasure()->noBreak();
        if (doBreak) {
            breakMeasure = ctx.mutState().curMeasure();
            system->removeLastMeasure();
            ctx.mutState().curMeasure()->setParent(oldSystem);
            while (ctx.state().prevMeasure() && ctx.state().prevMeasure()->noBreak() && system->measures().size() > 1) {
                ctx.mutState().setTick(ctx.state().tick() - ctx.state().curMeasure()->ticks());
                ctx.mutState().setMeasureNo(ctx.state().curMeasure()->no());

                ctx.mutState().setNextMeasure(ctx.mutState().curMeasure());
                ctx.mutState().setCurMeasure(ctx.mutState().prevMeasure());
                ctx.mutState().setPrevMeasure(ctx.mutState().curMeasure()->prev());

                system->removeLastMeasure();
                ctx.mutState().curMeasure()->setParent(oldSystem);
            }

            break;
        }

        if (oldSystem && system != oldSystem && muse::contains(ctx.state().systemList(), oldSystem)) {
            oldSystem->clear();
        }

        if (ctx.state().prevMeasure() && ctx.state().prevMeasure()->isMeasure() && ctx.state().prevMeasure()->system() == system) {
            Measure* m = toMeasure(ctx.mutState().prevMeasure());

            MeasureLayout::createEndBarLines(m, false, ctx);

            if (m->trailer()) {
                MeasureLayout::removeSystemTrailer(m, ctx);
            }

            MeasureLayout::setRepeatCourtesiesAndParens(m, ctx);

            MeasureLayout::updateGraceNotes(m, ctx);

            if (!systemLock) {
                curSysWidth = HorizontalSpacing::updateSpacingForLastAddedMeasure(system);
            }

            if (ctx.state().curMeasure()->isMeasure()) {
                MeasureLayout::updateKeySignatures(toMeasure(ctx.state().curMeasure()), ctx);
            }
        }

        const MeasureBase* mb = ctx.state().curMeasure();
        const MeasureBase* next = mb->nextMM();
        bool lineBreak  = false;
        switch (ctx.conf().viewMode()) {
        case LayoutMode::PAGE:
        case LayoutMode::SYSTEM:
            lineBreak = mb->pageBreak() || mb->lineBreak() || mb->sectionBreak() || mb->isEndOfSystemLock()
                        || (next && next->isStartOfSystemLock());
            break;
        case LayoutMode::FLOAT:
        case LayoutMode::LINE:
        case LayoutMode::HORIZONTAL_FIXED:
            lineBreak = false;
            break;
        }

        // preserve state of next measure (which is about to become current measure)
        if (ctx.state().nextMeasure()) {
            MeasureBase* nmb = ctx.mutState().nextMeasure();
            if (nmb->isMeasure() && ctx.conf().styleB(Sid::createMultiMeasureRests)) {
                Measure* nm = toMeasure(nmb);
                if (nm->hasMMRest()) {
                    nmb = nm->mmRest();
                }
            }
            if (nmb->isMeasure()) {
                prevMeasureState.clear();
                prevMeasureState.measure = toMeasure(nmb);
                prevMeasureState.measurePos = nmb->x();
                prevMeasureState.measureWidth = nmb->width();
                for (Segment& seg : toMeasure(nmb)->segments()) {
                    prevMeasureState.elementPositions.emplace(&seg, seg.ldata()->pos());
                    for (EngravingItem* item : seg.annotations()) {
                        if (item->isHarmony() || item->isFretDiagram()) {
                            prevMeasureState.elementPositions.emplace(item, item->ldata()->pos());
                        }
                    }
                }
            }
            if (!ctx.state().curMeasure()->noBreak()) {
                // current measure is not a nobreak,
                // so next measure could possibly start a system
                prevMeasureState.curHeader = nmb->header();
            }
            if (!nmb->noBreak()) {
                // next measure is not a nobreak
                // so it could possibly end a system
                prevMeasureState.curTrailer = nmb->trailer();
            }
        }

        MeasureLayout::getNextMeasure(ctx);

        // ElementType nt = lc.curMeasure ? lc.curMeasure->type() : ElementType::INVALID;
        mb = ctx.state().curMeasure();
        if (lineBreak || !mb || mb->isVBoxBase()) {
            break;
        }
    }

    assert(ctx.state().prevMeasure());

    if (ctx.state().endTick() < ctx.state().prevMeasure()->tick()) {
        // we've processed the entire range
        // but we need to continue layout until we reach a system whose last measure is the same as previous layout
        MeasureBase* curMB = ctx.mutState().curMeasure();
        Measure* m = curMB && curMB->isMeasure() ? toMeasure(curMB) : nullptr;
        bool curMeasureMayHaveJoinedBeams = m && BeamLayout::measureMayHaveBeamsJoinedIntoNext(m);
        if (ctx.state().prevMeasure() == ctx.state().systemOldMeasure() && !curMeasureMayHaveJoinedBeams) {
            // If current measure has possible beams joining to the next, we need to continue layout. This needs a better solution in future. [M.S.]
            // this system ends in the same place as the previous layout
            // ok to stop
            if (m) {
                // we may have previously processed first measure(s) of next system
                // so now we must restore to original state
                if (m->repeatStart()) {
                    Segment* s = m->findSegmentR(SegmentType::StartRepeatBarLine, Fraction(0, 1));
                    if (!s->enabled()) {
                        s->setEnabled(true);
                    }
                }
                const MeasureBase* pbmb = ctx.state().prevMeasure()->mbWithPrecedingSectionBreak();
                bool localFirstSystem = pbmb->sectionBreak() && !ctx.conf().isMode(LayoutMode::FLOAT);
                MeasureBase* nm = breakMeasure ? breakMeasure : m;
                if (prevMeasureState.curHeader) {
                    MeasureLayout::addSystemHeader(m, localFirstSystem, ctx);
                } else {
                    MeasureLayout::removeSystemHeader(m);
                }
                for (;;) {
                    // TODO: what if the nobreak group takes the entire system - is this correct?
                    if (prevMeasureState.curTrailer && !m->noBreak()) {
                        MeasureLayout::addSystemTrailer(m, m->nextMeasure(), ctx);
                    } else {
                        MeasureLayout::removeSystemTrailer(m, ctx);
                    }

                    MeasureLayout::setRepeatCourtesiesAndParens(m, ctx);

                    MeasureLayout::updateGraceNotes(m, ctx);

                    prevMeasureState.restoreMeasure();
                    MeasureLayout::layoutMeasureElements(m, ctx);
                    BeamLayout::restoreBeams(m, ctx);
                    SystemLayout::restoreOldSystemLayout(m->system(), ctx);
                    if (m == nm || !m->noBreak()) {
                        break;
                    }
                    m = m->nextMeasure();
                }
            }
            ctx.mutState().setRangeDone(true);
        }
    }

    if (ctx.dom().allStavesInvisible()) {
        // Edge case. Can only happen if all instruments have been deleted.
        return system;
    }

    /*************************************************************
     * SYSTEM NOW HAS A COMPLETE SET OF MEASURES
     * Now perform all operation to finalize system.
     * **********************************************************/

    // Break cross-measure beams
    if (ctx.state().prevMeasure() && ctx.state().prevMeasure()->isMeasure()) {
        Measure* pm = toMeasure(ctx.mutState().prevMeasure());
        BeamLayout::breakCrossMeasureBeams(pm, ctx);
    }

    // Hide empty staves
    hideEmptyStaves(system, ctx, ctx.state().firstSystem());

    // Re-create shapes to account for newly hidden/unhidden staves
    // (and for potential forgotten shape updates, for example in MeasureLayout::setRepeatCourtesiesAndParens)
    for (MeasureBase* mb : system->measures()) {
        if (mb->isMeasure()) {
            for (Segment& seg : toMeasure(mb)->segments()) {
                seg.createShapes();
            }
        }
    }

    // Relayout system to account for newly hidden/unhidden staves
    SystemLayout::layoutSystem(system, ctx, layoutSystemMinWidth, ctx.state().firstSystem(), ctx.state().firstSystemIndent());

    // Create end barlines and system trailer if needed (cautionary time/key signatures etc)
    Measure* lm  = system->lastMeasure();
    if (lm) {
        MeasureLayout::createEndBarLines(lm, true, ctx);
        Measure* nm = lm->nextMeasure();
        if (nm) {
            MeasureLayout::addSystemTrailer(lm, nm, ctx);
        }
    }

    updateTimeSigAboveStavesXPos(system, ctx);

    // Recompute spacing to account for the last changes (barlines, hidden staves, etc)
    curSysWidth = HorizontalSpacing::computeSpacingForFullSystem(system);

    if (curSysWidth > targetSystemWidth) {
        HorizontalSpacing::squeezeSystemToFit(system, curSysWidth, targetSystemWidth);
    }

    if (shouldBeJustified(system, curSysWidth, targetSystemWidth, ctx)) {
        HorizontalSpacing::justifySystem(system, curSysWidth, targetSystemWidth);
    }

    clearBigTimeSigNotShown(system, ctx);

    // LAYOUT MEASURES
    bool createBrackets = false;
    for (MeasureBase* mb : system->measures()) {
        if (mb->isMeasure()) {
            mb->setParent(system);
            Measure* m = toMeasure(mb);
            MeasureLayout::layoutMeasureElements(m, ctx);
            MeasureLayout::layoutStaffLines(m, ctx);
            if (createBrackets) {
                SystemLayout::addBrackets(system, toMeasure(mb), ctx);
                createBrackets = false;
            }
        } else if (mb->isHBox()) {
            HBox* curHBox = toHBox(mb);
            TLayout::layoutMeasureBase(curHBox, ctx);
            createBrackets = curHBox->createSystemHeader();
        }
    }

    layoutSystemElements(system, ctx);
    SystemLayout::layout2(system, ctx);     // compute staff distances

    if (ctx.state().rangeDone() && oldSystem && !oldSystem->measures().empty() && oldSystem->measures().front()->tick() >= system->endTick()
        && !(oldSystem->page() && oldSystem->page() != ctx.state().page())) {
        // We may have unfinished layouts of certain elements in the next system
        // - ties & bends (in LayoutChords::updateLineAttachPoints())
        // - fret diagrams & harmony (in layoutMeasure)
        // Restore them to the correct state.
        SystemLayout::restoreOldSystemLayout(oldSystem, ctx);
    }

    return system;
}

bool SystemLayout::shouldBeJustified(System* system, double curSysWidth, double targetSystemWidth, LayoutContext& ctx)
{
    bool shouldJustify = true;

    MeasureBase* lm = system->measures().back();
    if ((curSysWidth / targetSystemWidth) < ctx.conf().styleD(Sid::lastSystemFillLimit)) {
        shouldJustify = false;
        const MeasureBase* lastMb = ctx.state().curMeasure();

        // For systems with a section break, don't justify
        if (lm && lm->sectionBreak()) {
            lastMb = nullptr;
        }

        // Justify if system is followed by a measure or HBox
        while (lastMb) {
            if (lastMb->isMeasure() || lastMb->isHBox()) {
                shouldJustify = true;
                break;
            }
            // Frames can contain section breaks too, account for that here
            if (lastMb->sectionBreak()) {
                shouldJustify = false;
                break;
            }
            lastMb = lastMb->nextMeasure();
        }
    }

    return shouldJustify && !MScore::noHorizontalStretch;
}

void SystemLayout::layoutSystemLockIndicators(System* system, LayoutContext& ctx)
{
    UNUSED(ctx);

    const std::vector<SystemLockIndicator*> lockIndicators = system->lockIndicators();
    // In PAGE view, at most ONE lock indicator can exist per system.
    assert(lockIndicators.size() <= 1);
    system->deleteLockIndicators();

    const SystemLock* lock = system->systemLock();
    if (!lock) {
        return;
    }

    SystemLockIndicator* lockIndicator = new SystemLockIndicator(system, lock);
    lockIndicator->setParent(system);
    system->addLockIndicator(lockIndicator);

    TLayout::layoutIndicatorIcon(lockIndicator, lockIndicator->mutldata());
}

//---------------------------------------------------------
//   getNextSystem
//---------------------------------------------------------

System* SystemLayout::getNextSystem(LayoutContext& ctx)
{
    bool isVBox = ctx.state().curMeasure()->isVBox();
    System* system = nullptr;
    if (ctx.state().systemList().empty()) {
        system = Factory::createSystem(ctx.mutDom().dummyParent()->page());
        ctx.mutState().setSystemOldMeasure(nullptr);
    } else {
        system = muse::takeFirst(ctx.mutState().systemList());
        ctx.mutState().setSystemOldMeasure(system->measures().empty() ? 0 : system->measures().back());
        system->clear();       // remove measures from system
    }
    ctx.mutDom().systems().push_back(system);
    if (!isVBox) {
        size_t nstaves = ctx.dom().nstaves();
        system->adjustStavesNumber(nstaves);
        for (staff_idx_t i = 0; i < nstaves; ++i) {
            system->staff(i)->setShow(ctx.dom().staff(i)->show());
        }
    }
    return system;
}

enum class StaffHideMode {
    HIDE_WHEN_STAFF_EMPTY,
    HIDE_WHEN_INSTRUMENT_EMPTY,
    ALWAYS_SHOW
};

static StaffHideMode computeHideMode(const System* system, const Staff* staff, const staff_idx_t staffIdx, const bool globalHideIfEmpty,
                                     bool& hasSystemSpecificOverrides)
{
    // Check for system-specific overrides
    bool hasSystemSpecificOverrideHide = false;
    bool hasSystemSpecificOverrideDontHide = false;

    for (const MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }

        const Measure* measure = toMeasure(mb);
        const AutoOnOff hideIfEmpty = measure->hideStaffIfEmpty(staffIdx);

        hasSystemSpecificOverrideHide |= (hideIfEmpty == AutoOnOff::ON);
        hasSystemSpecificOverrideDontHide |= (hideIfEmpty == AutoOnOff::OFF);
    }

    if (hasSystemSpecificOverrideDontHide) {
        hasSystemSpecificOverrides = true;
        return StaffHideMode::ALWAYS_SHOW;
    } else if (hasSystemSpecificOverrideHide) {
        hasSystemSpecificOverrides = true;
        return StaffHideMode::HIDE_WHEN_STAFF_EMPTY;
    }

    // Consider staff setting
    AutoOnOff staffHideMode = staff->hideWhenEmpty();
    switch (staffHideMode) {
    case AutoOnOff::ON:
        return StaffHideMode::HIDE_WHEN_STAFF_EMPTY;
    case AutoOnOff::OFF:
        return StaffHideMode::ALWAYS_SHOW;
    case AutoOnOff::AUTO:
        break;
    }

    // Consider part setting
    AutoOnOff partHideMode = staff->part()->hideWhenEmpty();
    switch (partHideMode) {
    case AutoOnOff::ON:
        break;
    case AutoOnOff::OFF:
        return StaffHideMode::ALWAYS_SHOW;
    case AutoOnOff::AUTO:
        // Consider global setting
        if (!globalHideIfEmpty) {
            return StaffHideMode::ALWAYS_SHOW;
        }
    }

    return staff->part()->hideStavesWhenIndividuallyEmpty()
           ? StaffHideMode::HIDE_WHEN_STAFF_EMPTY
           : StaffHideMode::HIDE_WHEN_INSTRUMENT_EMPTY;
}

static bool computeShowSysStaff(const System* system, const Staff* staff, const staff_idx_t staffIdx,
                                const Fraction& stick, const SpannerMap::IntervalList& spanners,
                                const StaffHideMode hideMode)
{
    if (hideMode == StaffHideMode::ALWAYS_SHOW) {
        return true;
    }

    // Check if there are spanners
    for (const auto& spanner : spanners) {
        if (spanner.value->staff() == staff
            && !spanner.value->systemFlag()
            && !(spanner.stop == stick.ticks() && !spanner.value->isSlur())) {
            return true;
        }
    }

    // Check if the staff is empty in the system
    for (const MeasureBase* m : system->measures()) {
        if (!m->isMeasure()) {
            continue;
        }
        const Measure* measure = toMeasure(m);
        if (!measure->isEmpty(staffIdx)) {
            return true;
        }
    }

    // check if notes moved into this staff
    Part* part = staff->part();
    const size_t n = part->nstaves();
    if (n > 1) {
        staff_idx_t idx = part->staves().front()->idx();
        for (staff_idx_t i = 0; i < n; ++i) {
            staff_idx_t st = idx + i;

            for (MeasureBase* mb : system->measures()) {
                if (!mb->isMeasure()) {
                    continue;
                }

                const Measure* m = toMeasure(mb);
                bool empty = m->isEmpty(st);
                if (hideMode == StaffHideMode::HIDE_WHEN_INSTRUMENT_EMPTY && !empty) {
                    return true;
                } else if (empty) {
                    continue;
                }

                for (Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                    for (voice_idx_t voice = 0; voice < VOICES; ++voice) {
                        ChordRest* cr = s->cr(st * VOICES + voice);
                        int staffMove = cr ? cr->staffMove() : 0;
                        if (!cr || cr->isRest() || cr->staffMove() == 0) {
                            // The case staffMove == 0 has already been checked by measure->isEmpty()
                            continue;
                        }
                        if (staffIdx == st + staffMove) {
                            return true;
                        }
                    }
                }
            }
        }
    }

    return false;
}

void SystemLayout::hideEmptyStaves(System* system, LayoutContext& ctx, bool isFirstSystem)
{
    if (ctx.dom().nstaves() == 0) {
        // No staves, nothing to hide
        return;
    }

    if (ctx.dom().nstaves() == 1) {
        // One staff, show iff not manually hidden score-wide
        const bool show = ctx.dom().staves().front()->show();
        system->staves().front()->setShow(show);
        system->setHasStaffVisibilityIndicator(false);
        return;
    }

    const Fraction stick = system->first()->tick();
    const Fraction etick = system->last()->endTick();
    const auto& spanners = ctx.dom().spannerMap().findOverlapping(stick.ticks(), etick.ticks() - 1);

    const bool globalHideIfEmpty = ctx.conf().styleB(Sid::hideEmptyStaves)
                                   && !(isFirstSystem && ctx.conf().styleB(Sid::dontHideStavesInFirstSystem));

    bool hasSystemSpecificOverrides = false;
    bool hasHiddenStaves = false;
    bool systemIsEmpty = true;
    staff_idx_t staffIdx = 0;

    for (const Staff* staff : ctx.dom().staves()) {
        SysStaff* ss = system->staff(staffIdx);

        DEFER {
            ++staffIdx;
        };

        if (!staff->show()) {
            ss->setShow(false);
            continue;
        }

        const StaffHideMode hideMode = computeHideMode(system, staff, staffIdx, globalHideIfEmpty, hasSystemSpecificOverrides);
        const bool show = computeShowSysStaff(system, staff, staffIdx, stick, spanners, hideMode);
        ss->setShow(show);
        if (show) {
            systemIsEmpty = false;
        } else {
            hasHiddenStaves = true;
        }
    }

    system->setHasStaffVisibilityIndicator(hasSystemSpecificOverrides || hasHiddenStaves);

    // If the system is empty, unhide the staves with `showIfEntireSystemEmpty` set to true, if any
    const Staff* firstVisible = nullptr;
    if (systemIsEmpty) {
        for (const Staff* staff : ctx.dom().staves()) {
            SysStaff* ss  = system->staff(staff->idx());
            if (staff->showIfEntireSystemEmpty() && !ss->show()) {
                ss->setShow(true);
                systemIsEmpty = false;
            } else if (!firstVisible && staff->show()) {
                firstVisible = staff;
            }
        }
    }

    // If there are no such staves, unhide the first one
    if (systemIsEmpty) {
        const Staff* staff = firstVisible ? firstVisible : ctx.dom().staves().front();
        SysStaff* ss = system->staff(staff->idx());
        ss->setShow(true);
    }
}

bool SystemLayout::canChangeSysStaffVisibility(const System* system, const staff_idx_t staffIdx)
{
    if (system->staves().size() <= 1) {
        // Only one staff; always visible
        return false;
    }

    const Staff* staff = system->score()->staff(staffIdx);

    if (system->staff(staffIdx)->show()) {
        // SysStaff is visible; check if can hide
        const Fraction stick = system->first()->tick();
        const Fraction etick = system->last()->endTick();
        const auto& spanners = system->score()->spannerMap().findOverlapping(stick.ticks(), etick.ticks() - 1);

        return !computeShowSysStaff(system, staff, staffIdx, system->first()->tick(), spanners, StaffHideMode::HIDE_WHEN_STAFF_EMPTY);
    }

    // SysStaff is hidden; check if can show
    if (!staff->show()) {
        return false;
    }

    return true;
}

void SystemLayout::updateTimeSigAboveStavesXPos(System* system, LayoutContext& ctx)
{
    if (ctx.conf().styleV(Sid::timeSigPlacement).value<TimeSigPlacement>() != TimeSigPlacement::ABOVE_STAVES) {
        return;
    }

    staff_idx_t nstaves = ctx.dom().nstaves();
    bool centerOnBarline = ctx.conf().styleB(Sid::timeSigCenterOnBarline);

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* measure = toMeasure(mb);
        for (Segment& seg : measure->segments()) {
            if (!seg.isType(SegmentType::TimeSigType)) {
                continue;
            }

            std::set<TimeSig*> timeSigToKeep;
            for (staff_idx_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
                TimeSig* timeSig = toTimeSig(seg.element(staff2track(staffIdx)));
                if (!timeSig || !timeSig->showOnThisStaff()) {
                    continue;
                }

                timeSigToKeep.insert(timeSig);
                if (system->staff(staffIdx)->show()) {
                    continue;
                }

                staff_idx_t nextVisStaff = system->nextVisibleStaff(staffIdx);
                if (nextVisStaff == muse::nidx) {
                    continue;
                }

                TimeSig* nextVisTimeSig = toTimeSig(seg.element(staff2track(nextVisStaff)));
                if (nextVisTimeSig) {
                    timeSigToKeep.insert(nextVisTimeSig);
                }
            }

            Segment* prevBarlineSeg = nullptr;
            Segment* prevRepeatAnnounceTimeSigSeg = nullptr;
            if (centerOnBarline) {
                for (Segment* prevSeg = seg.prev1(); prevSeg && prevSeg->tick() == seg.tick(); prevSeg = prevSeg->prev1()) {
                    if (prevSeg->isEndBarLineType()) {
                        prevBarlineSeg = prevSeg;
                    } else if (prevSeg->isTimeSigRepeatAnnounceType()) {
                        prevRepeatAnnounceTimeSigSeg = prevSeg;
                    }
                }
            }

            for (staff_idx_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
                TimeSig* timeSig = toTimeSig(seg.element(staff2track(staffIdx)));
                if (!timeSig) {
                    continue;
                }
                Parenthesis* leftParen = timeSig->leftParen();
                Parenthesis* rightParen = timeSig->rightParen();
                if (!muse::contains(timeSigToKeep, timeSig)) {
                    timeSig->mutldata()->reset(); // Eliminates the shape
                    if (leftParen) {
                        leftParen->mutldata()->reset();
                    }
                    if (rightParen) {
                        rightParen->mutldata()->reset();
                    }
                    continue;
                }

                if (prevBarlineSeg && prevBarlineSeg->system() == system && !prevRepeatAnnounceTimeSigSeg) {
                    // Center timeSig on its segment
                    RectF bbox = timeSig->ldata()->bbox();
                    double newXPos = -0.5 * (bbox.right() + bbox.left());
                    timeSig->mutldata()->setPosX(newXPos);
                } else if (!seg.isTimeSigRepeatAnnounceType()) {
                    // Left-align to parenthesis if present
                    double xLeftParens = DBL_MAX;
                    if (leftParen) {
                        xLeftParens = leftParen->x() + leftParen->ldata()->bbox().left();
                    }
                    if (xLeftParens != DBL_MAX) {
                        timeSig->mutldata()->moveX(-xLeftParens);
                    }
                } else {
                    // TimeSigRepeatAnnounce: right-align to segment
                    double xRight = -DBL_MAX;
                    xRight = std::max(xRight, timeSig->shape().right() + timeSig->x());
                    if (leftParen) {
                        xRight = std::max(xRight, leftParen->ldata()->bbox().right() + leftParen->x());
                    }
                    if (rightParen) {
                        xRight = std::max(xRight, rightParen->ldata()->bbox().right() + rightParen->x());
                    }
                    if (xRight != -DBL_MAX) {
                        timeSig->mutldata()->moveX(-xRight);
                    }
                }
            }

            seg.createShapes();
        }
    }
}

void SystemLayout::clearBigTimeSigNotShown(System* system, LayoutContext& ctx)
{
    if (ctx.conf().styleV(Sid::timeSigPlacement).value<TimeSigPlacement>() == TimeSigPlacement::NORMAL) {
        return;
    }

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (Segment& seg : toMeasure(mb)->segments()) {
            if (!seg.isType(SegmentType::TimeSigType)) {
                continue;
            }
            for (staff_idx_t staffIdx = 0; staffIdx < ctx.dom().nstaves(); ++staffIdx) {
                TimeSig* ts = toTimeSig(seg.element(staff2track(staffIdx)));
                if (!ts) {
                    continue;
                }
                if (!ts->showOnThisStaff() || ts->effectiveStaffIdx() == muse::nidx) {
                    ts->mutldata()->reset(); // Deletes shape
                }
            }
        }
    }
}

void SystemLayout::layoutSticking(const std::vector<Sticking*> stickings, System* system, LayoutContext& ctx)
{
    if (stickings.empty()) {
        return;
    }

    for (Sticking* sticking : stickings) {
        TLayout::layoutItem(sticking, ctx);
    }

    std::vector<EngravingItem*> stickingItems(stickings.begin(), stickings.end());
    AlignmentLayout::alignItemsForSystem(stickingItems, system);
}

void SystemLayout::layoutLyrics(const ElementsToLayout& elements, LayoutContext& ctx)
{
    System* system = elements.system;
    // NOTE: in continuous view, this means we layout spanners for the entire score.
    // TODO: find way to optimize this and only layout where necessary.
    Fraction stick = system->measures().front()->tick();
    Fraction etick = system->measures().back()->endTick();

    for (Spanner* sp : elements.partialLyricsLines) {
        TLayout::layoutSystem(sp, system, ctx);
    }

    //-------------------------------------------------------------
    // Lyric
    //-------------------------------------------------------------
    // Layout lyrics dashes and melisma
    // NOTE: loop on a *copy* of unmanagedSpanners because in some cases
    // the underlying operation may invalidate some of the iterators.
    // TODO: figure out why lyrics lines are in this "unmanagedSpanners" container
    bool dashOnFirstNoteSyllable = ctx.conf().style().styleB(Sid::lyricsShowDashIfSyllableOnFirstNote);
    std::set<Spanner*> unmanagedSpanners = ctx.dom().unmanagedSpanners();
    for (Spanner* sp : unmanagedSpanners) {
        if (!sp->systemFlag() && sp->staff() && !sp->staff()->show()) {
            continue;
        }
        bool dashOnFirst = dashOnFirstNoteSyllable && !toLyricsLine(sp)->isEndMelisma();
        if (sp->tick() >= etick || sp->tick2() < stick || (sp->tick2() == stick && !dashOnFirst)) {
            continue;
        }
        TLayout::layoutSystem(sp, system, ctx);
    }
    LyricsLayout::computeVerticalPositions(system, ctx);
}

void SystemLayout::layoutVoltas(const ElementsToLayout& elementsToLayout, LayoutContext& ctx)
{
    System* system = elementsToLayout.system;

    processLines(system, ctx, elementsToLayout.voltas);

    std::set<Volta*> alignedVoltas;
    for (size_t i = 0; i < elementsToLayout.voltas.size(); ++i) {
        Volta* volta1 = toVolta(elementsToLayout.voltas[i]);
        if (alignedVoltas.count(volta1)) {
            continue;
        }

        std::vector<EngravingItem*> voltasToAlign;
        voltasToAlign.push_back(volta1->backSegment());

        for (size_t j = i + 1; j < elementsToLayout.voltas.size(); ++j) {
            Volta* volta2 = toVolta(elementsToLayout.voltas[j]);
            if (volta2->staffIdx() == volta1->staffIdx() && volta2->tick() == volta1->tick2()) {
                voltasToAlign.push_back(volta2->frontSegment());
                volta1 = volta2;
            }
        }

        if (voltasToAlign.size() > 1) {
            AlignmentLayout::alignItemsGroup(voltasToAlign, system);
            for (EngravingItem* vs : voltasToAlign) {
                alignedVoltas.insert(toVoltaSegment(vs)->volta());
            }
        }
    }
}

void SystemLayout::layoutDynamicExpressionAndHairpins(const ElementsToLayout& elementsToLayout, LayoutContext& ctx)
{
    System* system = elementsToLayout.system;

    std::vector<EngravingItem*> dynamicsExprAndHairpinsToAlign;

    for (Dynamic* dynamic : elementsToLayout.dynamics) {
        TLayout::layoutItem(dynamic, ctx);
        if (dynamic->autoplace()) {
            Autoplace::autoplaceSegmentElement(dynamic, dynamic->mutldata());
            dynamicsExprAndHairpinsToAlign.push_back(dynamic);
        }
    }

    for (Expression* e : elementsToLayout.expressions) {
        TLayout::layoutItem(e, ctx);
        if (e->addToSkyline()) {
            dynamicsExprAndHairpinsToAlign.push_back(e);
        }
    }

    processLines(system, ctx, elementsToLayout.hairpins);

    for (SpannerSegment* spannerSegment : system->spannerSegments()) {
        if (spannerSegment->isHairpinSegment()) {
            dynamicsExprAndHairpinsToAlign.push_back(spannerSegment);
        }
    }

    AlignmentLayout::alignItemsWithTheirSnappingChain(dynamicsExprAndHairpinsToAlign, system);

    if (MScore::pianomaniaPrettifySlursFingerings) {
        liftPianomaniaHairpinChainsClearOfSlurs(system, dynamicsExprAndHairpinsToAlign);
        movePianomaniaTextHairpinsClearOfNotation(system);
    }
}

void SystemLayout::layoutParenthesisAndBigTimeSigs(const ElementsToLayout& elementsToLayout)
{
    System* system = elementsToLayout.system;

    for (Parenthesis* e : elementsToLayout.parenthesis) {
        Segment* s = toSegment(e->parentItem());
        if (s->isType(SegmentType::TimeSigType)) {
            EngravingItem* el = s->element(e->track());
            TimeSig* timeSig = el ? toTimeSig(el) : nullptr;
            if (!timeSig) {
                continue;
            }
            TimeSigPlacement timeSigPlacement = timeSig->style().styleV(Sid::timeSigPlacement).value<TimeSigPlacement>();
            if (timeSigPlacement == TimeSigPlacement::ACROSS_STAVES) {
                if (!timeSig->showOnThisStaff()) {
                    e->mutldata()->reset();
                }
                continue;
            }
        }

        staff_idx_t si = e->staffIdx();
        Measure* m = s->measure();
        system->staff(si)->skyline().add(e->shape().translate(e->pos() + s->pos() + m->pos() + e->staffOffset()));
    }

    for (TimeSig* timeSig : elementsToLayout.timeSigAboveStaves) {
        Autoplace::autoplaceSegmentElement(timeSig, timeSig->mutldata());
    }
}

static void autoplaceHarmony(EngravingItem* harmony)
{
    FretDiagram* fdParent = harmony->parent()->isFretDiagram() ? toFretDiagram(harmony->parent()) : nullptr;
    if (fdParent && !fdParent->visible()) {
        harmony->mutldata()->moveY(-fdParent->pos().y());
    }
    Autoplace::autoplaceSegmentElement(harmony, harmony->mutldata());
}

void SystemLayout::layoutHarmonies(const std::vector<Harmony*> harmonies, System* system, LayoutContext& ctx)
{
    if (!ctx.conf().styleB(Sid::verticallyAlignChordSymbols)) {
        for (Harmony* harmony : harmonies) {
            autoplaceHarmony(harmony);
        }
        return;
    }

    // Only vertically align one chord symbol per tick & staff
    std::set<std::pair<Fraction, staff_idx_t> > harmonyPositions;
    std::vector<EngravingItem*> harmonyItemsAlign;
    std::vector<EngravingItem*> harmonyItemsNoAlign;

    for (Harmony* h : harmonies) {
        if (muse::contains(harmonyPositions, { h->tick(), h->staffIdx() })) {
            harmonyItemsNoAlign.push_back(h);
            continue;
        }

        TLayout::layoutHarmony(h, h->mutldata(), ctx);
        autoplaceHarmony(h);
        harmonyItemsAlign.push_back(h);
        harmonyPositions.insert({ h->tick(), h->staffIdx() });
    }

    AlignmentLayout::alignItemsForSystem(harmonyItemsAlign, system);

    for (EngravingItem* harmony : harmonyItemsNoAlign) {
        autoplaceHarmony(harmony);
    }
}

void SystemLayout::layoutFretDiagrams(const ElementsToLayout& elements, System* system, LayoutContext& ctx)
{
    if (!ctx.conf().styleB(Sid::verticallyAlignChordSymbols)) {
        for (FretDiagram* fretDiag : elements.fretDiagrams) {
            Autoplace::autoplaceSegmentElement(fretDiag, fretDiag->mutldata());
        }

        for (Harmony* harmony : elements.harmonies) {
            autoplaceHarmony(harmony);
        }

        for (FretDiagram* fretDiag : elements.fretDiagrams) {
            if (Harmony* harmony = fretDiag->harmony()) {
                SkylineLine& skl = system->staff(fretDiag->staffIdx())->skyline().north();
                Segment* s = fretDiag->segment();
                Shape harmShape = harmony->ldata()->shape().translated(harmony->pos() + fretDiag->pos() + s->pos() + s->measure()->pos());
                skl.add(harmShape);
            }
        }

        return;
    }

    // Only vertically align one fd per tick & staff
    std::set<std::pair<Fraction, staff_idx_t> > fretHarmonyPositions;
    std::vector<EngravingItem*> fretItemsAlign;
    std::vector<EngravingItem*> fretOrHarmonyItemsNoAlign;
    std::vector<Harmony*> harmonyItemsAlign(elements.harmonies.begin(), elements.harmonies.end());

    for (FretDiagram* fd : elements.fretDiagrams) {
        if (muse::contains(fretHarmonyPositions, { fd->tick(), fd->staffIdx() })) {
            fretOrHarmonyItemsNoAlign.push_back(fd);
            if (fd->harmony()) {
                harmonyItemsAlign.erase(std::remove(harmonyItemsAlign.begin(), harmonyItemsAlign.end(), fd->harmony()));
                fretOrHarmonyItemsNoAlign.push_back(fd->harmony());
            }
            continue;
        }

        TLayout::layoutFretDiagram(fd, fd->mutldata(), ctx);
        Autoplace::autoplaceSegmentElement(fd, fd->mutldata());
        fretItemsAlign.push_back(fd);
        fretHarmonyPositions.insert({ fd->tick(), fd->staffIdx() });
    }

    // Find harmony with no fret diagram at the same tick as a fret diagram
    for (Harmony* h : elements.harmonies) {
        if (h->getParentFretDiagram()) {
            continue;
        }
        if (muse::contains(fretHarmonyPositions, { h->tick(), h->staffIdx() })) {
            harmonyItemsAlign.erase(std::remove(harmonyItemsAlign.begin(), harmonyItemsAlign.end(), h));
            fretOrHarmonyItemsNoAlign.push_back(h);
            continue;
        }

        Autoplace::autoplaceSegmentElement(h, h->mutldata());
        fretHarmonyPositions.insert({ h->tick(), h->staffIdx() });
    }

    // align 1 fret diagram per tick & staff
    AlignmentLayout::alignItemsForSystem(fretItemsAlign, system);

    layoutHarmonies(harmonyItemsAlign, system, ctx);

    // autoplace everything else
    for (EngravingItem* item : fretOrHarmonyItemsNoAlign) {
        if (item->isFretDiagram()) {
            Autoplace::autoplaceSegmentElement(item, item->mutldata());
            Harmony* harmony = toFretDiagram(item)->harmony();
            if (harmony) {
                autoplaceHarmony(harmony);
            }
        } else if (item->isHarmony()) {
            autoplaceHarmony(item);
        }
    }
}

void SystemLayout::layoutSystemElements(System* system, LayoutContext& ctx)
{
    TRACEFUNC;

    if (ctx.dom().nstaves() == 0) {
        return;
    }

    ElementsToLayout elementsToLayout(system);

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        if (ctx.conf().isLinearMode() && (mb->tick() < ctx.state().startTick() || mb->tick() > ctx.state().endTick())) {
            // in continuous view, entire score is one system but we only need to process the range
            continue;
        }

        Measure* measure = toMeasure(mb);

        MeasureLayout::layoutMeasureNumber(measure, ctx);
        MeasureLayout::layoutMMRestRange(measure, ctx);
        MeasureLayout::layoutPlayCountText(measure, ctx);
        MeasureLayout::layoutTimeTickAnchors(measure, ctx);

        collectElementsToLayout(measure, elementsToLayout, ctx);
    }

    const std::vector<Segment*>& sl = elementsToLayout.segments;
    if (sl.empty()) {
        return;
    }

    for (Chord* chord : elementsToLayout.chords) {
        GraceNotesGroup& graceBefore = chord->graceNotesBefore();
        GraceNotesGroup& graceAfter = chord->graceNotesAfter();
        TLayout::layoutGraceNotesGroup2(&graceBefore, graceBefore.mutldata());
        TLayout::layoutGraceNotesGroup2(&graceAfter, graceAfter.mutldata());
    }

    for (ChordRest* cr : elementsToLayout.chordRests) {
        BeamLayout::layoutNonCrossBeams(cr, ctx);
    }

    RestLayout::alignRests(elementsToLayout.system, ctx);
    RestLayout::checkFullMeasureRestCollisions(elementsToLayout.system, ctx);

    for (BarLine* bl : elementsToLayout.barlines) {
        TLayout::updateBarlineShape(bl, bl->mutldata(), ctx);
    }

    createSkylines(elementsToLayout, ctx);

    layoutTiesAndBends(elementsToLayout, ctx);

    if (ctx.conf().isLinearMode()) {
        // TODO: get rid of this
        doLayoutNoteSpannersLinear(system, ctx);
    }

    for (Chord* c : elementsToLayout.chords) {
        ChordLayout::layoutArticulations(c, ctx);
        ChordLayout::layoutArticulations2(c, ctx);
        ChordLayout::layoutChordBaseFingering(c, system, ctx);
    }

    layoutTuplets(elementsToLayout.chordRests, ctx);

    collectSpannersToLayout(elementsToLayout, ctx);

    processLines(system, ctx, elementsToLayout.slurs);

    for (Spanner* sp : elementsToLayout.slurs) {
        Slur* slur = toSlur(sp);
        ChordRest* scr = toChordRest(slur->startElement());
        ChordRest* ecr = toChordRest(slur->endElement());
        if (scr && scr->isChord()) {
            ChordLayout::layoutArticulations3(toChord(scr), slur, ctx);
        }
        if (ecr && ecr->isChord()) {
            ChordLayout::layoutArticulations3(toChord(ecr), slur, ctx);
        }
        if (slur->isHammerOnPullOff()) {
            StaffType* staffType = slur->staff()->staffType(slur->tick());
            if ((staffType->isTabStaff() && ctx.conf().styleB(Sid::hopoAlignLettersTabStaves))
                || (!staffType->isTabStaff() && ctx.conf().styleB(Sid::hopoAlignLettersStandardStaves))) {
                AlignmentLayout::alignHopoLetters(toHammerOnPullOff(slur), system);
            }
        }
    }
    adjustPianomaniaFingeringsAroundNotation(system, /*addFinalRectsToSkylines=*/ true);

    processLines(system, ctx, elementsToLayout.trills);

    layoutSticking(elementsToLayout.stickings, system, ctx);

    for (EngravingItem* item : elementsToLayout.fermatasAndTremoloBars) {
        TLayout::layoutItem(item, ctx);
    }

    for (FiguredBass* item : elementsToLayout.figuredBass) {
        TLayout::layoutItem(item, ctx);
        if (item->autoplace()) {
            Autoplace::autoplaceSegmentElement(item, item->mutldata(), true);
        }
    }

    layoutDynamicExpressionAndHairpins(elementsToLayout, ctx);

    processLines(system, ctx, elementsToLayout.allOtherSpanners);

    for (MeasureNumber* mno : elementsToLayout.measureNumbers) {
        if (!mno->visible()) {
            continue;
        }
        Autoplace::autoplaceMeasureElement(mno, mno->mutldata());
        system->staff(mno->staffIdx())->skyline().add(mno->ldata()->bbox().translated(mno->measure()->pos() + mno->pos()
                                                                                      + mno->staffOffset()), mno);
    }

    for (MMRestRange* mmrr : elementsToLayout.mmrRanges) {
        Autoplace::autoplaceMeasureElement(mmrr, mmrr->mutldata());
        system->staff(mmrr->staffIdx())->skyline().add(mmrr->ldata()->bbox().translated(mmrr->measure()->pos() + mmrr->pos()), mmrr);
    }

    processLines(system, ctx, elementsToLayout.ottavas);
    processLines(system, ctx, elementsToLayout.pedal, /*align=*/ true);

    layoutLyrics(elementsToLayout, ctx);

    for (HarpPedalDiagram* hpd : elementsToLayout.harpDiagrams) {
        TLayout::layoutItem(hpd, ctx);
    }

    bool hasFretDiagram = elementsToLayout.fretDiagrams.size() > 0;
    if (!hasFretDiagram) {
        layoutHarmonies(elementsToLayout.harmonies, system, ctx);
    }

    for (StaffText* st : elementsToLayout.staffText) {
        TLayout::layoutItem(st, ctx);
    }

    for (InstrumentChange* ic : elementsToLayout.instrChanges) {
        TLayout::layoutItem(ic, ctx);
    }

    for (EngravingItem* item : elementsToLayout.playTechCapoStringTunTripletFeel) {
        TLayout::layoutItem(item, ctx);
    }

    if (hasFretDiagram) {
        layoutFretDiagrams(elementsToLayout, system, ctx);
    }

    for (SystemText* systemText : elementsToLayout.systemText) {
        TLayout::layoutSystemText(systemText, systemText->mutldata());
    }

    layoutVoltas(elementsToLayout, ctx);

    for (RehearsalMark* rm : elementsToLayout.rehMarks) {
        TLayout::layoutItem(rm, ctx);
    }

    std::vector<EngravingItem*> tempoElementsToAlign;
    for (TempoText* tt : elementsToLayout.tempoText) {
        TLayout::layoutItem(tt, ctx);
        tempoElementsToAlign.push_back(tt);
    }

    processLines(system, ctx, elementsToLayout.tempoChangeLines);
    for (SpannerSegment* spannerSeg : system->spannerSegments()) {
        if (spannerSeg->isGradualTempoChangeSegment()) {
            tempoElementsToAlign.push_back(spannerSeg);
        }
    }

    for (PlayCountText* pt : elementsToLayout.playCountText) {
        TLayout::layoutPlayCountText(pt, pt->mutldata());
        if (pt->autoplace()) {
            Autoplace::autoplaceSegmentElement(pt, pt->mutldata());
        }
    }

    AlignmentLayout::alignItemsWithTheirSnappingChain(tempoElementsToAlign, system);

    for (RehearsalMark* rehearsMark : elementsToLayout.rehMarks) {
        Autoplace::autoplaceSegmentElement(rehearsMark, rehearsMark->mutldata());
    }

    for (EngravingItem* item : elementsToLayout.markersAndJumps) {
        TLayout::layoutItem(item, ctx);
    }

    for (Image* image : elementsToLayout.images) {
        TLayout::layoutItem(image, ctx);
    }

    layoutParenthesisAndBigTimeSigs(elementsToLayout);
}

void SystemLayout::collectElementsToLayout(Measure* measure, ElementsToLayout& elements, const LayoutContext& ctx)
{
    elements.measures.push_back(measure);

    System* system = elements.system;
    for (size_t staffIdx = 0; staffIdx < ctx.dom().nstaves(); ++staffIdx) {
        if (measure->showMeasureNumberOnStaff(staffIdx)) {
            if (MeasureNumber* mno = measure->measureNumber(staffIdx)) {
                elements.measureNumbers.push_back(mno);
            }
        }

        if (!system->staff(staffIdx)->show()) {
            continue;
        }

        MMRestRange* mmrr = measure->mmRangeText(staffIdx);
        if (mmrr && mmrr->addToSkyline()) {
            elements.mmrRanges.push_back(mmrr);
        }

        for (EngravingItem* item : measure->el()) {
            if (item->effectiveStaffIdx() == staffIdx && (item->isMarker() || item->isJump())) {
                elements.markersAndJumps.push_back(item);
            }
        }
    }

    track_idx_t nTracks = ctx.dom().ntracks();
    for (Segment* s = measure->first(); s; s = s->next()) {
        if (s->isChordRestType() || !s->annotations().empty()) {
            elements.segments.push_back(s);
        }

        for (track_idx_t track = 0; track < nTracks; /* intentionally empty*/ ) {
            if (s->hasTimeSigAboveStaves()) {
                TimeSig* timeSig = toTimeSig(s->element(track));
                if (timeSig && timeSig->showOnThisStaff()) {
                    elements.timeSigAboveStaves.push_back(timeSig);
                }
                track += VOICES;
                continue;
            }

            if (!system->staff(track2staff(track))->show()) {
                track += VOICES;
                continue;
            }

            if (s->isType(SegmentType::BarLineType)) {
                if (BarLine* bl = toBarLine(s->element(track))) {
                    elements.barlines.push_back(bl);
                }
                track += VOICES;
                continue;
            }

            if (s->isChordRestType()) {
                if (ChordRest* cr = toChordRest(s->element(track))) {
                    elements.chordRests.push_back(cr);
                    if (cr->isChord()) {
                        elements.chords.push_back(toChord(cr));

                        auto collectBends = [&elements] (Chord* chord) {
                            for (Note* note : chord->notes()) {
                                for (Spanner* sp : note->spannerBack()) {
                                    if (sp->isGuitarBend()) {
                                        elements.guitarBends.push_back(toGuitarBend(sp));
                                    }
                                }
                                if (GuitarBend* bendFor = note->bendFor(); bendFor && bendFor->bendType() == GuitarBendType::SLIGHT_BEND) {
                                    elements.guitarBends.push_back(bendFor);
                                }
                            }
                        };

                        Chord* chord = toChord(cr);
                        for (Chord* grace : chord->graceNotesBefore()) {
                            collectBends(grace);
                        }
                        collectBends(chord);
                        for (Chord* grace : chord->graceNotesAfter()) {
                            collectBends(grace);
                        }
                    }
                }
                ++track;
                continue;
            }

            ++track;
        }

        for (EngravingItem* item : s->annotations()) {
            if (!item->systemFlag() && !system->staff(item->staffIdx())->show()) {
                continue;
            }
            switch (item->type()) {
            case ElementType::STICKING:
                elements.stickings.push_back(toSticking(item));
                break;
            case ElementType::FERMATA:
            case ElementType::TREMOLOBAR:
                elements.fermatasAndTremoloBars.push_back(item);
                break;
            case ElementType::FIGURED_BASS:
                elements.figuredBass.push_back(toFiguredBass(item));
                break;
            case ElementType::DYNAMIC:
                elements.dynamics.push_back(toDynamic(item));
                break;
            case ElementType::EXPRESSION:
                elements.expressions.push_back(toExpression(item));
                break;
            case ElementType::HARP_DIAGRAM:
                elements.harpDiagrams.push_back(toHarpPedalDiagram(item));
                break;
            case ElementType::FRET_DIAGRAM:
                elements.fretDiagrams.push_back(toFretDiagram(item));
                if (Harmony* h = toFretDiagram(item)->harmony()) {
                    elements.harmonies.push_back(h);
                }
                break;
            case ElementType::STAFF_TEXT:
                elements.staffText.push_back(toStaffText(item));
                break;
            case ElementType::INSTRUMENT_CHANGE:
                elements.instrChanges.push_back(toInstrumentChange(item));
                break;
            case ElementType::PLAYTECH_ANNOTATION:
            case ElementType::CAPO:
            case ElementType::STRING_TUNINGS:
            case ElementType::TRIPLET_FEEL:
                elements.playTechCapoStringTunTripletFeel.push_back(item);
                break;
            case ElementType::SYSTEM_TEXT:
                elements.systemText.push_back(toSystemText(item));
                break;
            case ElementType::REHEARSAL_MARK:
                elements.rehMarks.push_back(toRehearsalMark(item));
                break;
            case ElementType::TEMPO_TEXT:
                elements.tempoText.push_back(toTempoText(item));
                break;
            case ElementType::IMAGE:
                elements.images.push_back(toImage(item));
                break;
            case ElementType::PARENTHESIS:
                elements.parenthesis.push_back(toParenthesis(item));
                break;
            case ElementType::HARMONY:
                elements.harmonies.push_back(toHarmony(item));
                break;
            case ElementType::PLAY_COUNT_TEXT:
                elements.playCountText.push_back(toPlayCountText(item));
                break;
            default:
                break;
            }
        }
    }
}

void SystemLayout::collectSpannersToLayout(ElementsToLayout& elements, const LayoutContext& ctx)
{
    const System* system = elements.system;

    // NOTE: in continuous view, this means we layout spanners for the entire score.
    // TODO: find way to optimize this and only layout where necessary.
    Fraction stick = system->measures().front()->tick();
    Fraction etick = system->measures().back()->endTick();

    auto spanners = ctx.dom().spannerMap().findOverlapping(stick.ticks(), etick.ticks());
    std::sort(spanners.begin(), spanners.end(), [](const auto& sp1, const auto& sp2) {
        return sp1.value->tick() < sp2.value->tick();
    });

    std::vector<Spanner*> allSpanners;
    allSpanners.reserve(spanners.size());
    for (auto item : spanners) {
        allSpanners.push_back(item.value);
    }

    for (Spanner* spanner : allSpanners) {
        if (!spanner->systemFlag() && !system->staff(spanner->staffIdx())->show()) {
            continue;
        }
        if (spanner->tick() >= etick || spanner->tick2() < stick) {
            continue;
        }

        if (spanner->tick2() == stick) {
            // Only these two spanner types are laid out if at the end of the previous system
            // TODO: pedal makes sense, but slur doesn't... to figure out
            if (spanner->isSlur() && !toSlur(spanner)->isCrossStaff() && !toSlur(spanner)->hasCrossBeams()) {
                elements.slurs.push_back(spanner);
            } else if (spanner->isPedal() && toPedal(spanner)->connect45HookToNext()) {
                elements.pedal.push_back(spanner);
            }
        } else {
            switch (spanner->type()) {
            case ElementType::SLUR:
            case ElementType::HAMMER_ON_PULL_OFF:
                if (!toSlur(spanner)->isCrossStaff() && !toSlur(spanner)->hasCrossBeams()) {
                    elements.slurs.push_back(spanner);
                }
                break;
            case ElementType::PEDAL:
                elements.pedal.push_back(spanner);
                break;
            case ElementType::TRILL:
                elements.trills.push_back(spanner);
                break;
            case ElementType::VOLTA:
                elements.voltas.push_back(spanner);
                break;
            case ElementType::HAIRPIN:
                elements.hairpins.push_back(spanner);
                break;
            case ElementType::GRADUAL_TEMPO_CHANGE:
                elements.tempoChangeLines.push_back(spanner);
                break;
            case ElementType::PARTIAL_LYRICSLINE:
                elements.partialLyricsLines.push_back(spanner);
                break;
            case ElementType::OTTAVA:
                if (!spanner->staff()->staffType()->isTabStaff()) {
                    elements.ottavas.push_back(spanner);
                }
                break;
            default:
                elements.allOtherSpanners.push_back(spanner);
                break;
            }
        }
    }
}

void SystemLayout::createSkylines(const ElementsToLayout& elementsToLayout, LayoutContext& ctx)
{
    System* system = elementsToLayout.system;
    for (size_t staffIdx = 0; staffIdx < ctx.dom().nstaves(); ++staffIdx) {
        SysStaff* ss = system->staff(staffIdx);
        Skyline& skyline = ss->skyline();
        skyline.clear();
        for (Measure* m : elementsToLayout.measures) {
            if (m->staffLines(staffIdx)->addToSkyline()) {
                ss->skyline().add(m->staffLines(staffIdx)->ldata()->bbox().translated(m->pos()), m->staffLines(staffIdx));
            }
            for (Segment& s : m->segments()) {
                if (!s.enabled()) {
                    continue;
                }
                PointF p(s.pos() + m->pos());
                if (s.isType(SegmentType::BarLineType)) {
                    BarLine* bl = toBarLine(s.element(staffIdx * VOICES));
                    if (bl && bl->addToSkyline()) {
                        skyline.add(bl->shape().translated(bl->pos() + p + bl->staffOffset()));
                    }
                } else if (s.isType(SegmentType::TimeSigType)) {
                    TimeSig* ts = toTimeSig(s.element(staffIdx * VOICES));
                    if (ts && ts->addToSkyline() && ts->showOnThisStaff()) {
                        TimeSigPlacement timeSigPlacement = ts->style().styleV(Sid::timeSigPlacement).value<TimeSigPlacement>();
                        if (timeSigPlacement != TimeSigPlacement::ACROSS_STAVES) {
                            skyline.add(ts->shape().translate(ts->pos() + p + ts->staffOffset()));
                        }
                    }
                } else {
                    track_idx_t strack = staffIdx * VOICES;
                    track_idx_t etrack = strack + VOICES;
                    for (EngravingItem* e : s.elist()) {
                        if (!e) {
                            continue;
                        }
                        track_idx_t effectiveTrack = e->vStaffIdx() * VOICES + e->voice();
                        if (effectiveTrack < strack || effectiveTrack >= etrack) {
                            continue;
                        }

                        // add element to skyline
                        if (e->addToSkyline()) {
                            const PointF offset = e->staffOffset();
                            Shape shape = e->shape();
                            // add grace notes to skyline
                            if (e->isChord()) {
                                Chord* chord = toChord(e);
                                GraceNotesGroup& graceBefore = chord->graceNotesBefore();
                                GraceNotesGroup& graceAfter = chord->graceNotesAfter();
                                if (!graceBefore.empty()) {
                                    skyline.add(graceBefore.shape().translate(graceBefore.pos() + p + offset));
                                }
                                if (!graceAfter.empty()) {
                                    skyline.add(graceAfter.shape().translate(graceAfter.pos() + p + offset));
                                }

                                // If present, add ornament cue note to skyline
                                Ornament* ornament = chord->findOrnament();
                                if (ornament) {
                                    Chord* cue = ornament->cueNoteChord();
                                    if (cue && cue->upNote()->visible()) {
                                        skyline.add(cue->shape().translate(cue->pos() + p + cue->staffOffset()));
                                    }
                                }

                                // Don't include cross-staff arpeggios
                                shape.remove_if([chord](ShapeElement& s) {
                                    return (s.item()->isArpeggio() || s.item()->isChordBracket()) && toArpeggio(
                                        s.item()) == chord->spanArpeggio();
                                });
                                Arpeggio* arp = chord->spanArpeggio();
                                if (arp) {
                                    RectF staffBbox = ss->bbox();
                                    RectF arpBbox = arp->ldata()->bbox().translated(e->pos() + p + offset);
                                    if (chord->track() == arp->track()) {
                                        staffBbox.setTop(arpBbox.top());
                                    } else if (chord->track() == arp->endTrack()) {
                                        staffBbox.setBottom(arpBbox.bottom());
                                    }
                                    shape.add(arpBbox & staffBbox, arp);
                                }
                            }
                            skyline.add(shape.translate(e->pos() + p + offset));
                        }

                        // add tremolo to skyline
                        if (e->isChord()) {
                            Chord* ch = item_cast<Chord*>(e);
                            // tremoloSingleChord is added directly to chord shape
                            if (ch->tremoloTwoChord()) {
                                TremoloTwoChord* t = ch->tremoloTwoChord();
                                Chord* c1 = t->chord1();
                                Chord* c2 = t->chord2();
                                if (c1 && !c1->staffMove() && c2 && !c2->staffMove()) {
                                    if (t->chord() == e && t->addToSkyline()) {
                                        skyline.add(t->shape().translate(t->pos() + e->pos() + p));
                                    }
                                }
                            }
                        }

                        // add beams to skline
                        if (e->isChordRest()) {
                            ChordRest* cr = toChordRest(e);
                            if (BeamLayout::isStartOfNonCrossBeam(cr)) {
                                Beam* b = cr->beam();
                                b->addSkyline(skyline);
                            }
                        }
                    }
                }
            }
        }
    }
}

void SystemLayout::doLayoutTies(System* system, const std::vector<Segment*>& sl, const Fraction& stick, const Fraction& etick,
                                LayoutContext& ctx)
{
    UNUSED(etick);

    for (Segment* s : sl) {
        for (EngravingItem* e : s->elist()) {
            if (!e || !e->isChord()) {
                continue;
            }
            Chord* c = toChord(e);
            for (Chord* ch : c->graceNotes()) {
                layoutTies(ch, system, stick, ctx);
            }
            layoutTies(c, system, stick, ctx);
        }
    }
}

void SystemLayout::layoutTuplets(const std::vector<ChordRest*>& chordRests, LayoutContext& ctx)
{
    std::set<Tuplet*> laidoutTuplets;
    for (auto revIter = chordRests.rbegin(); revIter != chordRests.rend(); ++revIter) {
        ChordRest* cr = *revIter;
        Tuplet* tuplet = cr->topTuplet();

        if (!tuplet || muse::contains(laidoutTuplets, tuplet)) {
            continue;
        }

        TupletLayout::layoutTupletAndNestedTuplets(tuplet, ctx); // this lays out also the inner tuplets
        laidoutTuplets.insert(tuplet);
    }
}

void SystemLayout::layoutTiesAndBends(const ElementsToLayout& elementsToLayout, LayoutContext& ctx)
{
    System* system = elementsToLayout.system;
    if (elementsToLayout.measures.empty()) {
        return;
    }
    Fraction stick = elementsToLayout.measures.front()->tick();

    for (Chord* chord : elementsToLayout.chords) {
        for (Chord* grace : chord->graceNotesBefore()) {
            layoutTies(grace, system, stick, ctx);
        }
        layoutTies(chord, system, stick, ctx);
        for (Chord* grace : chord->graceNotesAfter()) {
            layoutTies(grace, system, stick, ctx);
        }
    }

    GuitarDiveLayout::updateDiveSequences(elementsToLayout.guitarBends, ctx);

    for (GuitarBend* bend : elementsToLayout.guitarBends) {
        TLayout::layoutGuitarBend(bend, ctx);
    }
}

void SystemLayout::layoutNoteAnchoredSpanners(System* system, Chord* chord)
{
    // Add all spanners attached to notes, otherwise these will be removed if outside of the layout range
    for (Note* note : chord->notes()) {
        for (Spanner* spanner : note->spannerFor()) {
            for (SpannerSegment* spannerSeg : spanner->spannerSegments()) {
                spannerSeg->setSystem(system);
            }
        }
    }
}

void SystemLayout::doLayoutNoteSpannersLinear(System* system, LayoutContext& ctx)
{
    constexpr Fraction start = Fraction(0, 1);
    for (Measure* measure = system->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            if (!segment->isChordRestType()) {
                continue;
            }
            for (EngravingItem* e : segment->elist()) {
                if (!e || !e->isChord()) {
                    continue;
                }
                Chord* c = toChord(e);
                for (Chord* ch : c->graceNotes()) {
                    layoutTies(ch, system, start, ctx);
                    layoutNoteAnchoredSpanners(system, ch);
                }
                layoutTies(c, system, start, ctx);
                layoutNoteAnchoredSpanners(system, c);
            }
        }
    }
}

void SystemLayout::processLines(System* system, LayoutContext& ctx, const std::vector<Spanner*>& lines, bool align)
{
    std::vector<SpannerSegment*> segments;
    for (Spanner* sp : lines) {
        SpannerSegment* ss = TLayout::layoutSystem(sp, system, ctx);        // create/layout spanner segment for this system
        if (ss->autoplace()) {
            segments.push_back(ss);
        }
    }

    if (align && segments.size() > 1) {
        const size_t nstaves = system->staves().size();
        const double defaultY = segments[0]->ldata()->pos().y();
        std::vector<double> yAbove(nstaves, -DBL_MAX);
        std::vector<double> yBelow(nstaves, -DBL_MAX);

        for (SpannerSegment* ss : segments) {
            if (ss->visible()) {
                double& staffY = ss->spanner() && ss->spanner()->placeAbove() ? yAbove[ss->staffIdx()] : yBelow[ss->staffIdx()];
                staffY = std::max(staffY, ss->ldata()->pos().y());
            }
        }
        for (SpannerSegment* ss : segments) {
            if (!ss->isStyled(Pid::OFFSET)) {
                continue;
            }
            const double& staffY = ss->spanner() && ss->spanner()->placeAbove() ? yAbove[ss->staffIdx()] : yBelow[ss->staffIdx()];
            if (staffY > -DBL_MAX) {
                ss->mutldata()->setPosY(staffY);
            } else {
                ss->mutldata()->setPosY(defaultY);
            }
        }
    }

    if (segments.size() > 0 && segments.front()->isSlurSegment()) {
        SlurTieLayout::adjustOverlappingSlurs(system->spannerSegments());
    }

    //
    // Fix harmonic marks and vibrato overlaps
    //
    SpannerSegment* prevSegment = nullptr;
    bool fixed = false;

    for (SpannerSegment* ss : segments) {
        if (fixed) {
            fixed = false;
            prevSegment = ss;
            continue;
        }
        if (prevSegment) {
            if (prevSegment->visible()
                && ss->visible()
                && prevSegment->isHarmonicMarkSegment()
                && ss->isVibratoSegment()
                && muse::RealIsEqual(prevSegment->x(), ss->x())) {
                double diff = ss->ldata()->bbox().bottom() - prevSegment->ldata()->bbox().bottom()
                              + prevSegment->ldata()->bbox().top();
                prevSegment->mutldata()->moveY(diff);
                fixed = true;
            }
            if (prevSegment->visible()
                && ss->visible()
                && prevSegment->isVibratoSegment()
                && ss->isHarmonicMarkSegment()
                && muse::RealIsEqual(prevSegment->x(), ss->x())) {
                double diff = prevSegment->ldata()->bbox().bottom() - ss->ldata()->bbox().bottom()
                              + ss->ldata()->bbox().top();
                ss->mutldata()->moveY(diff);
                fixed = true;
            }
        }

        prevSegment = ss;
    }

    //
    // add shapes to skyline
    //
    for (SpannerSegment* ss : segments) {
        if (ss->addToSkyline()) {
            staff_idx_t stfIdx = ss->effectiveStaffIdx();
            if (stfIdx == muse::nidx) {
                continue;
            }
            system->staff(stfIdx)->skyline().add(ss->shape().translate(ss->pos()));
            if (ss->isHammerOnPullOffSegment()) {
                TLayout::layoutHammerOnPullOffSegment(toHammerOnPullOffSegment(ss), ctx);
            }
        }
    }
}

void SystemLayout::layoutTies(Chord* ch, System* system, const Fraction& stick, LayoutContext& ctx)
{
    SysStaff* staff = system->staff(ch->staffIdx());
    if (!staff->show()) {
        return;
    }
    std::vector<TieSegment*> stackedForwardTies;
    std::vector<TieSegment*> stackedBackwardTies;
    for (Note* note : ch->notes()) {
        Tie* t = note->tieFor();
        if (t && !t->isLaissezVib()) {
            TieSegment* ts = SlurTieLayout::layoutTieFor(t, system);
            if (ts && ts->addToSkyline()) {
                staff->skyline().add(ts->shape().translate(ts->pos()));
                stackedForwardTies.push_back(ts);
            }
        }
        t = note->tieBack();
        if (t) {
            if (note->incomingPartialTie() || t->startNote()->tick() < stick) {
                TieSegment* ts = SlurTieLayout::layoutTieBack(t, system, ctx);
                if (ts && ts->addToSkyline()) {
                    staff->skyline().add(ts->shape().translate(ts->pos()));
                    stackedBackwardTies.push_back(ts);
                }
            }
        }
    }

    SlurTieLayout::layoutLaissezVibChord(ch, ctx);

    if (!ch->staffType()->isTabStaff()) {
        SlurTieLayout::resolveVerticalTieCollisions(stackedForwardTies);
        SlurTieLayout::resolveVerticalTieCollisions(stackedBackwardTies);
    }
}

bool SystemLayout::measureHasCrossStuffOrModifiedBeams(const Measure* measure)
{
    for (const Segment& seg : measure->segments()) {
        if (!seg.isChordRestType()) {
            continue;
        }
        for (const EngravingItem* e : seg.elist()) {
            if (!e || !e->isChordRest()) {
                continue;
            }
            const Beam* beam = toChordRest(e)->beam();
            if (beam && (beam->cross() || beam->userModified())) {
                return true;
            }
            const Chord* c = e->isChord() ? toChord(e) : nullptr;
            if (c && c->tremoloTwoChord()) {
                const TremoloTwoChord* trem = c->tremoloTwoChord();
                const Chord* c1 = trem->chord1();
                const Chord* c2 = trem->chord2();
                if (trem->userModified() || c1->staffMove() != c2->staffMove()) {
                    return true;
                }
            }
            if (e->isChord() && !toChord(e)->graceNotes().empty()) {
                for (const Chord* grace : toChord(e)->graceNotes()) {
                    if (grace->beam() && (grace->beam()->cross() || grace->beam()->userModified())) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

void SystemLayout::updateCrossBeams(System* system, LayoutContext& ctx)
{
    LAYOUT_CALL() << LAYOUT_ITEM_INFO(system);

    SystemLayout::layout2(system, ctx); // Computes staff distances, essential for the rest of the calculations
    // Update grace cross beams
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (Segment& seg : toMeasure(mb)->segments()) {
            if (!seg.isChordRestType()) {
                continue;
            }
            for (EngravingItem* e : seg.elist()) {
                if (!e || !e->isChord()) {
                    continue;
                }
                for (Chord* grace : toChord(e)->graceNotes()) {
                    if (grace->beam() && (grace->beam()->cross() || grace->beam()->userModified())
                        && grace->beam()->elements().front() == grace) {
                        BeamLayout::layout(grace->beam(), ctx);
                    }
                }
            }
        }
    }
    // Update normal chords cross beams and respective segments
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        bool somethingChanged = false;
        for (Segment& seg : toMeasure(mb)->segments()) {
            if (!seg.isChordRestType()) {
                continue;
            }
            for (EngravingItem* e : seg.elist()) {
                if (!e || !e->isChord()) {
                    continue;
                }
                Chord* chord = toChord(e);
                if (chord->beam() && (chord->beam()->cross() || chord->beam()->userModified())
                    && chord->beam()->elements().front() == chord) {
                    bool prevUp = chord->up();
                    Stem* stem = chord->stem();
                    double prevStemLength = stem ? stem->length() : 0.0;
                    BeamLayout::layout(chord->beam(), ctx);
                    if (chord->up() != prevUp || (stem && stem->length() != prevStemLength)) {
                        // If the chord has changed direction needs to be re-laid out
                        ChordLayout::layoutChords1(ctx, &seg, chord->vStaffIdx());
                        somethingChanged = true;
                    }
                } else if (chord->tremoloTwoChord()) {
                    TremoloTwoChord* t = chord->tremoloTwoChord();
                    Chord* c1 = t->chord1();
                    Chord* c2 = t->chord2();
                    if (t->userModified() || (c1->staffMove() != 0 || c2->staffMove() != 0)) {
                        bool prevUp = chord->up();
                        ChordLayout::computeUp(chord, ctx);
                        if (chord->up() != prevUp) {
                            ChordLayout::layoutChords1(ctx, &seg, chord->vStaffIdx());
                            somethingChanged = true;
                        }
                    }
                }
            }
            if (somethingChanged) {
                seg.createShapes();
            }
        }
    }
}

void SystemLayout::restoreOldSystemLayout(System* system, LayoutContext& ctx)
{
    ElementsToLayout elements(system);
    for (MeasureBase* mb : system->measures()) {
        if (mb->isMeasure()) {
            collectElementsToLayout(toMeasure(mb), elements, ctx);
        }
    }

    layoutTiesAndBends(elements, ctx);
}

void SystemLayout::layoutSystem(System* system, LayoutContext& ctx, double xo1, const bool isFirstSystem, bool firstSystemIndent)
{
    if (system->staves().empty()) {                 // ignore vbox
        return;
    }

    // Get standard instrument name distance
    double instrumentNameOffset = ctx.conf().styleMM(Sid::instrumentNameOffset);
    // Now scale it depending on the text size (which also may not follow staff scaling)
    double textSizeScaling = 1.0;
    double actualSize = 0.0;
    double defaultSize = 0.0;
    bool followStaffSize = true;
    if (ctx.state().startWithLongNames()) {
        actualSize = ctx.conf().styleD(Sid::longInstrumentFontSize);
        defaultSize = DefaultStyle::defaultStyle().value(Sid::longInstrumentFontSize).toDouble();
        followStaffSize = ctx.conf().styleB(Sid::longInstrumentFontSpatiumDependent);
    } else {
        actualSize = ctx.conf().styleD(Sid::shortInstrumentFontSize);
        defaultSize = DefaultStyle::defaultStyle().value(Sid::shortInstrumentFontSize).toDouble();
        followStaffSize = ctx.conf().styleB(Sid::shortInstrumentFontSpatiumDependent);
    }
    textSizeScaling = actualSize / defaultSize;
    if (!followStaffSize) {
        textSizeScaling *= DefaultStyle::defaultStyle().value(Sid::spatium).toDouble() / ctx.conf().styleD(Sid::spatium);
    }
    textSizeScaling = std::max(textSizeScaling, 1.0);
    instrumentNameOffset *= textSizeScaling;

    size_t nstaves = system->staves().size();

    //---------------------------------------------------
    //  find x position of staves
    //---------------------------------------------------
    SystemLayout::layoutBrackets(system, ctx);
    double maxBracketsWidth = SystemLayout::totalBracketOffset(ctx);

    double maxNamesWidth = SystemLayout::instrumentNamesWidth(system, ctx, isFirstSystem);

    double indent = maxNamesWidth > 0 ? maxNamesWidth + instrumentNameOffset : 0.0;
    if (isFirstSystem && firstSystemIndent) {
        indent = std::max(indent, system->styleP(Sid::firstSystemIndentationValue) * system->mag() - maxBracketsWidth);
        maxNamesWidth = indent - instrumentNameOffset;
    }

    if (muse::RealIsNull(indent)) {
        if (ctx.conf().styleB(Sid::alignSystemToMargin)) {
            system->setLeftMargin(0.0);
        } else {
            system->setLeftMargin(maxBracketsWidth);
        }
    } else {
        system->setLeftMargin(indent + maxBracketsWidth);
    }

    for (size_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
        SysStaff* s = system->staves().at(staffIdx);
        const Staff* staff = ctx.dom().staff(staffIdx);
        if (!staff->show() || !s->show()) {
            s->setbbox(RectF());
            continue;
        }

        double staffMag = staff->staffMag(Fraction(0, 1));         // ??? TODO
        int staffLines = staff->lines(Fraction(0, 1));
        if (staffLines <= 1) {
            double h = staff->lineDistance(Fraction(0, 1)) * staffMag * system->spatium();
            s->setbbox(system->leftMargin() + xo1, -h, 0.0, 2 * h);
        } else {
            double h = (staffLines - 1) * staff->lineDistance(Fraction(0, 1));
            h = h * staffMag * system->spatium();
            s->setbbox(system->leftMargin() + xo1, 0.0, 0.0, h);
        }
    }

    //---------------------------------------------------
    //  layout brackets
    //---------------------------------------------------

    system->setBracketsXPosition(xo1 + system->leftMargin());

    //---------------------------------------------------
    //  layout instrument names x position
    //     at this point it is not clear which staves will
    //     be hidden, so layout all instrument names
    //---------------------------------------------------

    for (const SysStaff* s : system->staves()) {
        for (InstrumentName* t : s->instrumentNames) {
            TLayout::layoutInstrumentName(t, t->mutldata());

            switch (t->align().horizontal) {
            case AlignH::JUSTIFY:   // Justify is not supported for instrument names
            case AlignH::LEFT:
                t->mutldata()->setPosX(0);
                break;
            case AlignH::HCENTER:
                t->mutldata()->setPosX(maxNamesWidth * .5);
                break;
            case AlignH::RIGHT:
                t->mutldata()->setPosX(maxNamesWidth);
                break;
            }
        }
    }

    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        Measure* m = toMeasure(mb);
        if (m == system->measures().front() || (m->prev() && m->prev()->isHBox())) {
            MeasureLayout::createSystemBeginBarLine(m, ctx);
        }
    }
}

double SystemLayout::instrumentNamesWidth(System* system, LayoutContext& ctx, bool isFirstSystem)
{
    double namesWidth = 0.0;

    for (staff_idx_t staffIdx = 0; staffIdx < ctx.dom().nstaves(); ++staffIdx) {
        const SysStaff* staff = system->staff(staffIdx);
        if (!staff || (isFirstSystem && !staff->show())) {
            continue;
        }

        for (InstrumentName* name : staff->instrumentNames) {
            TLayout::layoutInstrumentName(name, name->mutldata());
            namesWidth = std::max(namesWidth, name->width());
        }
    }

    return namesWidth;
}

/// Calculates the total width of all brackets together that
/// would be visible when all staves are visible.
/// The logic in this method is closely related to the logic in
/// System::layoutBrackets and System::createBracket.
double SystemLayout::totalBracketOffset(LayoutContext& ctx)
{
    if (ctx.state().totalBracketsWidth() >= 0) {
        return ctx.state().totalBracketsWidth();
    }

    size_t columns = 0;
    for (const Staff* staff : ctx.dom().staves()) {
        for (const BracketItem* bi : staff->brackets()) {
            columns = std::max(columns, bi->column() + 1);
        }
    }

    size_t nstaves = ctx.dom().nstaves();
    std::vector < double > bracketWidth(nstaves, 0.0);
    for (staff_idx_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
        const Staff* staff = ctx.dom().staff(staffIdx);
        for (auto bi : staff->brackets()) {
            if (bi->bracketType() == BracketType::NO_BRACKET || !bi->visible()) {
                continue;
            }

            //! This logic is partially copied from System::createBracket.
            //! Of course, we don't need to worry about invisible staves,
            //! but we do need to worry about brackets that span past the
            //! last staff.
            staff_idx_t firstStaff = staffIdx;
            staff_idx_t lastStaff = staffIdx + bi->bracketSpan() - 1;
            if (lastStaff >= nstaves) {
                lastStaff = nstaves - 1;
            }

            for (; firstStaff <= lastStaff; ++firstStaff) {
                if (ctx.dom().staff(firstStaff)->show()) {
                    break;
                }
            }
            for (; lastStaff >= firstStaff; --lastStaff) {
                if (ctx.dom().staff(lastStaff)->show()) {
                    break;
                }
            }

            size_t span = lastStaff - firstStaff + 1;
            if (span > 1
                || (bi->bracketSpan() == span)
                || (span == 1 && ctx.conf().styleB(Sid::alwaysShowBracketsWhenEmptyStavesAreHidden))) {
                Bracket* dummyBr = Factory::createBracket(ctx.mutDom().dummyParent(), /*isAccessibleEnabled=*/ false);
                dummyBr->setBracketItem(bi);
                dummyBr->setStaffSpan(firstStaff, lastStaff);
                dummyBr->mutldata()->bracketHeight.set_value(3.5 * dummyBr->spatium() * 2); // default
                TLayout::layoutBracket(dummyBr, dummyBr->mutldata(), ctx.conf());
                for (staff_idx_t stfIdx = firstStaff; stfIdx <= lastStaff; ++stfIdx) {
                    bracketWidth[stfIdx] += dummyBr->ldata()->bracketWidth();
                }
                delete dummyBr;
            }
        }
    }

    double totalBracketsWidth = 0.0;
    for (double w : bracketWidth) {
        totalBracketsWidth = std::max(totalBracketsWidth, w);
    }
    ctx.mutState().setTotalBracketsWidth(totalBracketsWidth);

    return totalBracketsWidth;
}

double SystemLayout::layoutBrackets(System* system, LayoutContext& ctx)
{
    size_t nstaves = system->staves().size();
    size_t columns = system->getBracketsColumnsCount();

    std::vector<double> bracketWidth(columns, 0.0);

    std::vector<Bracket*> bl;
    bl.swap(system->brackets());

    for (size_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
        const Staff* s = ctx.dom().staff(staffIdx);
        for (size_t i = 0; i < columns; ++i) {
            for (auto bi : s->brackets()) {
                if (bi->column() != i || bi->bracketType() == BracketType::NO_BRACKET) {
                    continue;
                }
                Bracket* b = SystemLayout::createBracket(system, ctx, bi, i, static_cast<int>(staffIdx), bl, system->firstMeasure());
                if (b != nullptr) {
                    b->mutldata()->bracketHeight.set_value(3.5 * b->spatium() * 2); // dummy
                    TLayout::layoutBracket(b, b->mutldata(), ctx.conf());
                    bracketWidth[i] = std::max(bracketWidth[i], b->ldata()->bracketWidth());
                }
            }
        }
    }

    for (Bracket* b : bl) {
        delete b;
    }

    double totalBracketWidth = 0.0;

    if (!system->brackets().empty()) {
        for (double w : bracketWidth) {
            totalBracketWidth += w;
        }
    }

    return totalBracketWidth;
}

void SystemLayout::addBrackets(System* system, Measure* measure, LayoutContext& ctx)
{
    if (system->staves().empty()) {                 // ignore vbox
        return;
    }

    size_t nstaves = system->staves().size();

    //---------------------------------------------------
    //  find x position of staves
    //    create brackets
    //---------------------------------------------------

    size_t columns = system->getBracketsColumnsCount();

    std::vector<Bracket*> bl;
    bl.swap(system->brackets());

    for (staff_idx_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
        const Staff* s = ctx.dom().staff(staffIdx);
        for (size_t i = 0; i < columns; ++i) {
            for (auto bi : s->brackets()) {
                if (bi->column() != i || bi->bracketType() == BracketType::NO_BRACKET) {
                    continue;
                }
                SystemLayout::createBracket(system, ctx, bi, i, staffIdx, bl, measure);
            }
        }
        if (!system->staff(staffIdx)->show()) {
            continue;
        }
    }

    //---------------------------------------------------
    //  layout brackets
    //---------------------------------------------------
    SystemLayout::layoutBracketsVertical(system, ctx);

    system->setBracketsXPosition(measure->x());

    muse::join(system->brackets(), bl);
}

//---------------------------------------------------------
//   createBracket
//---------------------------------------------------------

Bracket* SystemLayout::createBracket(System* system, LayoutContext& ctx, BracketItem* bi, size_t column, staff_idx_t staffIdx,
                                     std::vector<Bracket*>& bl,
                                     Measure* measure)
{
    if (!measure) {
        return nullptr;
    }

    size_t nstaves = system->staves().size();
    staff_idx_t firstStaff = staffIdx;
    staff_idx_t lastStaff = staffIdx + bi->bracketSpan() - 1;
    if (lastStaff >= nstaves) {
        lastStaff = nstaves - 1;
    }

    for (; firstStaff <= lastStaff; ++firstStaff) {
        if (system->staff(firstStaff)->show()) {
            break;
        }
    }
    for (; lastStaff >= firstStaff; --lastStaff) {
        if (system->staff(lastStaff)->show()) {
            break;
        }
    }
    size_t span = lastStaff - firstStaff + 1;
    //
    // do not show bracket if it only spans one
    // system due to some invisible staves
    //
    if (span > 1
        || (bi->bracketSpan() == span)
        || (span == 1 && ctx.conf().styleB(Sid::alwaysShowBracketsWhenEmptyStavesAreHidden)
            && bi->bracketType() != BracketType::SQUARE)
        || (span == 1 && ctx.conf().styleB(Sid::alwaysShowSquareBracketsWhenEmptyStavesAreHidden)
            && bi->bracketType() == BracketType::SQUARE)) {
        //
        // this bracket is visible
        //
        Bracket* b = 0;
        track_idx_t track = staffIdx * VOICES;
        for (size_t k = 0; k < bl.size(); ++k) {
            if (bl[k]->track() == track && bl[k]->column() == column && bl[k]->bracketType() == bi->bracketType()
                && bl[k]->measure() == measure) {
                b = muse::takeAt(bl, k);
                break;
            }
        }
        if (b == 0) {
            b = Factory::createBracket(ctx.mutDom().dummyParent());
            b->setBracketItem(bi);
            b->setGenerated(true);
            b->setTrack(track);
            b->setMeasure(measure);
        }
        system->add(b);

        if (bi->selected()) {
            bool needSelect = true;

            std::vector<EngravingItem*> brackets = ctx.selection().elements(ElementType::BRACKET);
            for (const EngravingItem* element : brackets) {
                if (toBracket(element)->bracketItem() == bi) {
                    needSelect = false;
                    break;
                }
            }

            if (needSelect) {
                ctx.select(b, SelectType::ADD);
            }
        }

        b->setStaffSpan(firstStaff, lastStaff);

        return b;
    }

    return nullptr;
}

//---------------------------------------------------------
//   layout2
//    called after measure layout
//    adjusts staff distance
//---------------------------------------------------------

void SystemLayout::layout2(System* system, LayoutContext& ctx)
{
    TRACEFUNC;
    LAYOUT_CALL() << LAYOUT_ITEM_INFO(system);

    Box* vb = system->vbox();
    if (vb) {
        BoxLayout::layoutBox(vb, vb->mutldata(), ctx);
        system->setbbox(vb->ldata()->bbox());
        return;
    }

    system->setPos(0.0, 0.0);
    std::vector<std::pair<size_t, SysStaff*> > visibleStaves;

    for (size_t i = 0; i < system->staves().size(); ++i) {
        const Staff* s  = ctx.dom().staff(i);
        SysStaff* ss = system->staves().at(i);
        if (s->show() && ss->show()) {
            visibleStaves.push_back(std::pair<size_t, SysStaff*>(i, ss));
        } else {
            ss->setbbox(RectF());        // already done in layout() ?
        }
    }

    double _spatium            = system->spatium();
    double y                   = 0.0;
    double minVerticalDistance = ctx.conf().styleMM(Sid::minVerticalDistance);
    double staffDistance       = ctx.conf().styleMM(Sid::staffDistance);
    double akkoladeDistance    = ctx.conf().styleMM(Sid::akkoladeDistance);
    if (ctx.conf().isVerticalSpreadEnabled()) {
        staffDistance       = ctx.conf().styleMM(Sid::minStaffSpread);
        akkoladeDistance    = ctx.conf().styleMM(Sid::minStaffSpread);
    }

    if (visibleStaves.empty()) {
        return;
    }

    for (auto i = visibleStaves.begin();; ++i) {
        SysStaff* ss  = i->second;
        staff_idx_t si1 = i->first;
        const Staff* staff  = ctx.dom().staff(si1);
        auto ni = std::next(i);

        double dist = staff->staffHeight();
        double yOffset;
        double h;
        if (staff->lines(Fraction(0, 1)) == 1) {
            yOffset = _spatium * BARLINE_SPAN_1LINESTAFF_TO * 0.5;
            h = _spatium * (BARLINE_SPAN_1LINESTAFF_TO - BARLINE_SPAN_1LINESTAFF_FROM) * 0.5;
        } else {
            yOffset = 0.0;
            h = staff->staffHeight();
        }
        if (ni == visibleStaves.end()) {
            ss->setYOff(yOffset);
            ss->setbbox(system->leftMargin(), y - yOffset, system->width() - system->leftMargin(), h);
            ss->saveLayout();
            break;
        }

        staff_idx_t si2 = ni->first;
        const Staff* staff2  = ctx.dom().staff(si2);

        if (staff->part() == staff2->part()) {
            Measure* m = system->firstMeasure();
            double mag = m ? staff->staffMag(m->tick()) : 1.0;
            dist += akkoladeDistance * mag;
        } else {
            dist += staffDistance;
        }
        dist += staff2->absoluteFromSpatium(staff2->userDist());
        bool fixedSpace = false;
        for (const MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure()) {
                continue;
            }
            const Measure* m = toMeasure(mb);
            Spacer* sp = m->vspacerDown(si1);
            if (sp) {
                if (sp->spacerType() == SpacerType::FIXED) {
                    dist = staff->staffHeight(m->tick()) + sp->absoluteGap();
                    fixedSpace = true;
                    break;
                } else {
                    dist = std::max(dist, staff->staffHeight(m->tick()) + sp->absoluteGap());
                }
            }
            sp = m->vspacerUp(si2);
            if (sp) {
                dist = std::max(dist, staff->staffHeight(m->tick()) + sp->absoluteGap());
            }
        }
        if (!fixedSpace) {
            // check minimum distance to next staff
            // note that in continuous view, we normally only have a partial skyline for the system
            // a full one is only built when triggering a full layout
            // therefore, we don't know the value we get from minDistance will actually be enough
            // so we remember the value between layouts and increase it when necessary
            // (the first layout on switching to continuous view gives us good initial values)
            // the result is space is good to start and grows as needed
            // it does not, however, shrink when possible - only by trigger a full layout
            // (such as by toggling to page view and back)
            const double minHorizontalClearance = ctx.conf().styleMM(Sid::skylineMinHorizontalClearance);
            double d = ss->skyline().minDistance(system->System::staff(si2)->skyline(), minHorizontalClearance);
            if (ctx.conf().isLineMode()) {
                double previousDist = ss->continuousDist();
                if (d > previousDist) {
                    ss->setContinuousDist(d);
                } else {
                    d = previousDist;
                }
            }
            dist = std::max(dist, d + minVerticalDistance);
            dist = std::max(dist, minVertSpaceForCrossStaffBeams(system, si1, si2, ctx));
        }
        ss->setYOff(yOffset);
        ss->setbbox(system->leftMargin(), y - yOffset, system->width() - system->leftMargin(), h);
        ss->saveLayout();
        y += dist;
    }

    system->setSystemHeight(system->staff(visibleStaves.back().first)->bbox().bottom());
    system->setHeight(system->systemHeight());

    SystemLayout::setMeasureHeight(system, system->systemHeight(), ctx);

    //---------------------------------------------------
    //  layout brackets vertical position
    //---------------------------------------------------

    SystemLayout::layoutBracketsVertical(system, ctx);

    //---------------------------------------------------
    //  layout instrument names
    //---------------------------------------------------

    SystemLayout::layoutInstrumentNames(system, ctx);
}

double SystemLayout::minVertSpaceForCrossStaffBeams(System* system, staff_idx_t staffIdx1, staff_idx_t staffIdx2, LayoutContext& ctx)
{
    double minSpace = -DBL_MAX;
    track_idx_t startTrack = staffIdx1 * VOICES;
    track_idx_t endTrack = staffIdx2 * VOICES + VOICES;
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (Segment& segment : toMeasure(mb)->segments()) {
            if (!segment.isChordRestType()) {
                continue;
            }
            for (track_idx_t track = startTrack; track < endTrack; ++track) {
                EngravingItem* item = segment.element(track);
                if (!item || !item->isChord()) {
                    continue;
                }
                Beam* beam = toChord(item)->beam();
                if (!beam || !beam->autoplace() || beam->elements().front() != item) {
                    continue;
                }
                if (beam->ldata()->crossStaffBeamPos != BeamBase::CrossStaffBeamPosition::BETWEEN) {
                    continue;
                }
                const Chord* limitingChordAbove = nullptr;
                const Chord* limitingChordBelow = nullptr;
                double limitFromAbove = -DBL_MAX;
                double limitFromBelow = DBL_MAX;
                for (const ChordRest* cr : beam->elements()) {
                    if (!cr->isChord()) {
                        continue;
                    }
                    const Chord* chord = toChord(cr);
                    double minStemLength = BeamTremoloLayout::minStemLength(chord, beam->ldata()) * (chord->spatium() / 4);
                    bool isUnderCrossBeam = cr->isBelowCrossBeam(beam);
                    if (isUnderCrossBeam && chord->vStaffIdx() == staffIdx2) {
                        const Note* topNote = chord->upNote();
                        double noteLimit = topNote->y() - minStemLength;
                        if (noteLimit < limitFromBelow) {
                            limitFromBelow = noteLimit;
                            limitingChordBelow = chord;
                        }
                        limitFromBelow = std::min(limitFromBelow, noteLimit);
                    } else if (!isUnderCrossBeam && chord->vStaffIdx() == staffIdx1) {
                        const Note* bottomNote = chord->downNote();
                        double noteLimit = bottomNote->y() + minStemLength;
                        if (noteLimit > limitFromAbove) {
                            limitFromAbove = noteLimit;
                            limitingChordAbove = chord;
                        }
                    }
                }
                if (limitingChordAbove && limitingChordBelow) {
                    double minSpaceRequired = limitFromAbove - limitFromBelow;
                    beam->mutldata()->limitingChordAbove = limitingChordAbove;
                    beam->mutldata()->limitingChordBelow = limitingChordBelow;

                    const BeamSegment* topBeamSegmentForChordAbove = beam->topLevelSegmentForElement(limitingChordAbove);
                    const BeamSegment* topBeamSegmentForChordBelow = beam->topLevelSegmentForElement(limitingChordBelow);
                    if (topBeamSegmentForChordAbove->above == topBeamSegmentForChordBelow->above) {
                        // In this case the two opposing stems overlap the beam height, so we must subtract it
                        int strokeCount = std::min(BeamTremoloLayout::strokeCount(beam->ldata(), limitingChordAbove),
                                                   BeamTremoloLayout::strokeCount(beam->ldata(), limitingChordBelow));
                        double beamHeight = ctx.conf().styleMM(Sid::beamWidth).val() + beam->beamDist() * (strokeCount - 1);
                        minSpaceRequired -= beamHeight;
                    }
                    minSpace = std::max(minSpace, minSpaceRequired);
                }
            }
        }
    }

    return minSpace;
}

void SystemLayout::restoreLayout2(System* system, LayoutContext& ctx)
{
    TRACEFUNC;
    if (system->vbox()) {
        return;
    }

    for (SysStaff* s : system->staves()) {
        s->restoreLayout();
    }

    system->setHeight(system->systemHeight());
    SystemLayout::setMeasureHeight(system, system->systemHeight(), ctx);
}

void SystemLayout::setMeasureHeight(System* system, double height, const LayoutContext& ctx)
{
    double spatium = system->spatium();
    for (MeasureBase* m : system->measures()) {
        MeasureBase::LayoutData* mldata = m->mutldata();
        if (m->isMeasure()) {
            // note that the factor 2 * _spatium must be corrected for when exporting
            // system distance in MusicXML (issue #24733)
            mldata->setBbox(0.0, -spatium, m->width(), height + 2.0 * spatium);
        } else if (m->isHBox()) {
            mldata->setBbox(m->absoluteFromSpatium(toHBox(m)->topGap()), 0.0, m->width(), height);
            BoxLayout::layoutHBox2(toHBox(m), ctx);
        } else if (m->isTBox()) {
            BoxLayout::layoutTBox(toTBox(m), toTBox(m)->mutldata(), ctx);
        } else if (m->isFBox()) {
            BoxLayout::layoutFBox(toFBox(m), toFBox(m)->mutldata(), ctx);
        } else {
            LOGD("unhandled measure type %s", m->typeName());
        }
    }
}

void SystemLayout::layoutBracketsVertical(System* system, LayoutContext& ctx)
{
    for (Bracket* b : system->brackets()) {
        int staffIdx1 = static_cast<int>(b->firstStaff());
        int staffIdx2 = static_cast<int>(b->lastStaff());
        double sy = 0;                           // assume bracket not visible
        double ey = 0;
        // if start staff not visible, try next staff
        while (staffIdx1 <= staffIdx2 && !system->staves().at(staffIdx1)->show()) {
            ++staffIdx1;
        }
        // if end staff not visible, try prev staff
        while (staffIdx1 <= staffIdx2 && !system->staves().at(staffIdx2)->show()) {
            --staffIdx2;
        }
        // if the score doesn't have "alwaysShowBracketsWhenEmptyStavesAreHidden" as true,
        // the bracket will be shown IF:
        // it spans at least 2 visible staves (staffIdx1 < staffIdx2) OR
        // it spans just one visible staff (staffIdx1 == staffIdx2) but it is required to do so
        // (the second case happens at least when the bracket is initially dropped)
        bool notHidden = ctx.conf().styleB(Sid::alwaysShowBracketsWhenEmptyStavesAreHidden)
                         ? (staffIdx1 <= staffIdx2) : (staffIdx1 < staffIdx2) || (b->span() == 1 && staffIdx1 == staffIdx2);
        if (notHidden) {                        // set vert. pos. and height to visible spanned staves
            sy = system->staves().at(staffIdx1)->bbox().top();
            ey = system->staves().at(staffIdx2)->bbox().bottom();
        }

        Bracket::LayoutData* bldata = b->mutldata();
        bldata->setPosY(sy);
        bldata->bracketHeight = ey - sy;
        TLayout::layoutBracket(b, bldata, ctx.conf());
    }
}

void SystemLayout::layoutInstrumentNames(System* system, LayoutContext& ctx)
{
    staff_idx_t staffIdx = 0;

    for (const Part* p : ctx.dom().parts()) {
        SysStaff* s = system->staff(staffIdx);
        SysStaff* s2;
        size_t nstaves = p->nstaves();

        staff_idx_t visible = system->firstVisibleSysStaffOfPart(p);
        if (visible != muse::nidx) {
            // The top staff might be invisible but this top staff contains the instrument names.
            // To make sure these instrument name are drawn, even when the top staff is invisible,
            // move the InstrumentName elements to the first visible staff of the part.
            if (visible != staffIdx) {
                SysStaff* vs = system->staff(visible);
                for (InstrumentName* t : s->instrumentNames) {
                    t->setTrack(visible * VOICES);
                    t->setSysStaff(vs);
                    vs->instrumentNames.push_back(t);
                }
                s->instrumentNames.clear();
                s = vs;
            }

            for (InstrumentName* t : s->instrumentNames) {
                //
                // override Text->layout()
                //
                double y1, y2;
                switch (t->layoutPos()) {
                default:
                case 0:                         // center at part
                    y1 = s->bbox().top();
                    s2 = system->staff(staffIdx);
                    for (int i = static_cast<int>(staffIdx + nstaves - 1); i > 0; --i) {
                        SysStaff* s3 = system->staff(i);
                        if (s3->show()) {
                            s2 = s3;
                            break;
                        }
                    }
                    y2 = s2->bbox().bottom();
                    break;
                case 1:                         // center at first staff
                    y1 = s->bbox().top();
                    y2 = s->bbox().bottom();
                    break;
                case 2:                         // center between first and second staff
                    y1 = s->bbox().top();
                    y2 = system->staff(staffIdx + 1)->bbox().bottom();
                    break;
                case 3:                         // center at second staff
                    y1 = system->staff(staffIdx + 1)->bbox().top();
                    y2 = system->staff(staffIdx + 1)->bbox().bottom();
                    break;
                case 4:                         // center between first and second staff
                    y1 = system->staff(staffIdx + 1)->bbox().top();
                    y2 = system->staff(staffIdx + 2)->bbox().bottom();
                    break;
                case 5:                         // center at third staff
                    y1 = system->staff(staffIdx + 2)->bbox().top();
                    y2 = system->staff(staffIdx + 2)->bbox().bottom();
                    break;
                }
                t->mutldata()->setPosY(y1 + (y2 - y1) * .5 + t->offset().y());
            }
        }
        staffIdx += nstaves;
    }
}

void SystemLayout::setInstrumentNames(System* system, LayoutContext& ctx, bool longName, Fraction tick)
{
    //
    // remark: add/remove instrument names is not undo/redoable
    //         as add/remove of systems is not undoable
    //
    if (system->vbox()) {                 // ignore vbox
        return;
    }
    if (!ctx.conf().isShowInstrumentNames()
        || (ctx.conf().styleB(Sid::hideInstrumentNameIfOneInstrument) && ctx.dom().visiblePartCount() <= 1)
        || (ctx.state().firstSystem()
            && ctx.conf().styleV(Sid::firstSystemInstNameVisibility).value<InstrumentLabelVisibility>() == InstrumentLabelVisibility::HIDE)
        || (!ctx.state().firstSystem()
            && ctx.conf().styleV(Sid::subsSystemInstNameVisibility).value<InstrumentLabelVisibility>()
            == InstrumentLabelVisibility::HIDE)) {
        for (SysStaff* staff : system->staves()) {
            for (InstrumentName* t : staff->instrumentNames) {
                ctx.mutDom().removeElement(t);
            }
        }
        return;
    }

    int staffIdx = 0;
    for (SysStaff* staff : system->staves()) {
        const Staff* s = ctx.dom().staff(staffIdx);
        Part* part = s->part();

        bool atLeastOneVisibleStaff = false;
        for (Staff* partStaff : part->staves()) {
            if (partStaff->show()) {
                atLeastOneVisibleStaff = true;
                break;
            }
        }

        bool showName = part->show() && atLeastOneVisibleStaff;
        if (!s->isTop() || !showName) {
            for (InstrumentName* t : staff->instrumentNames) {
                ctx.mutDom().removeElement(t);
            }
            ++staffIdx;
            continue;
        }

        const StaffNameList& names = longName ? part->longNames(tick) : part->shortNames(tick);

        size_t idx = 0;
        for (const StaffName& sn : names) {
            InstrumentName* iname = muse::value(staff->instrumentNames, idx);
            if (iname == 0) {
                iname = new InstrumentName(system);
                iname->setGenerated(true);
                iname->setParent(system);
                iname->setSysStaff(staff);
                iname->setTrack(staffIdx * VOICES);
                iname->setInstrumentNameType(longName ? InstrumentNameType::LONG : InstrumentNameType::SHORT);
                iname->setLayoutPos(sn.pos());
                ctx.mutDom().addElement(iname);
            }
            iname->setXmlText(sn.name());
            ++idx;
        }
        for (; idx < staff->instrumentNames.size(); ++idx) {
            ctx.mutDom().removeElement(staff->instrumentNames[idx]);
        }
        ++staffIdx;
    }
}

//---------------------------------------------------------
//   minDistance
//    Return the minimum distance between this system and s2
//    without any element collisions.
//
//    top - top system
//    bottom   - bottom system
//---------------------------------------------------------

double SystemLayout::minDistance(const System* top, const System* bottom, const LayoutContext& ctx)
{
    TRACEFUNC;

    const LayoutConfiguration& conf = ctx.conf();
    const DomAccessor& dom = ctx.dom();

    const VBox* topVBox = static_cast<VBox*>(top->vbox());
    const VBox* bottomVBox = static_cast<VBox*>(bottom->vbox());

    if (topVBox && !bottomVBox) {
        return std::max(topVBox->absoluteFromSpatium(topVBox->bottomGap()),
                        bottom->minTop() + topVBox->absoluteFromSpatium(topVBox->paddingToNotationBelow()));
    } else if (!topVBox && bottomVBox) {
        return std::max(bottomVBox->absoluteFromSpatium(bottomVBox->topGap()),
                        top->minBottom() + bottomVBox->absoluteFromSpatium(bottomVBox->paddingToNotationAbove()));
    } else if (topVBox && bottomVBox) {
        const double topToBottomGap = topVBox->absoluteFromSpatium(topVBox->bottomGap());
        const double bottomToTopGap = bottomVBox->absoluteFromSpatium(bottomVBox->topGap());
        if (topToBottomGap >= 0 && bottomToTopGap >= 0) {
            double largestGap = std::max(bottomToTopGap, topToBottomGap);
            return largestGap;
        } else {
            return topToBottomGap + bottomToTopGap;
        }
    }

    if (top->staves().empty() || bottom->staves().empty()) {
        return 0.0;
    }

    double minVerticalDistance = conf.styleMM(Sid::minVerticalDistance);
    double dist = conf.isVerticalSpreadEnabled() ? conf.styleMM(Sid::minSystemSpread) : conf.styleMM(Sid::minSystemDistance);
    size_t firstStaff = 0;
    size_t lastStaff = 0;

    for (firstStaff = 0; firstStaff < top->staves().size() - 1; ++firstStaff) {
        if (dom.staff(firstStaff)->show() && bottom->staff(firstStaff)->show()) {
            break;
        }
    }
    for (lastStaff = top->staves().size() - 1; lastStaff > 0; --lastStaff) {
        if (dom.staff(lastStaff)->show() && top->staff(lastStaff)->show()) {
            break;
        }
    }

    const Staff* staff = dom.staff(firstStaff);
    double userDist = staff ? staff->absoluteFromSpatium(staff->userDist()) : 0.0;
    dist = std::max(dist, userDist);
    top->setFixedDownDistance(false);

    const SysStaff* sysStaff = top->staff(lastStaff);
    const double minHorizontalClearance = conf.styleMM(Sid::skylineMinHorizontalClearance);
    double sld = sysStaff ? sysStaff->skyline().minDistance(bottom->staff(firstStaff)->skyline(), minHorizontalClearance) : 0;
    sld -= sysStaff ? sysStaff->bbox().height() - minVerticalDistance : 0;

    if (conf.isFloatMode()) {
        return std::max(dist, sld);
    }

    for (const MeasureBase* mb1 : top->measures()) {
        if (mb1->isMeasure()) {
            const Measure* m = toMeasure(mb1);
            const Spacer* sp = m->vspacerDown(lastStaff);
            if (sp) {
                if (sp->spacerType() == SpacerType::FIXED) {
                    dist = sp->absoluteGap();
                    top->setFixedDownDistance(true);
                    break;
                } else {
                    dist = std::max(dist, sp->absoluteGap());
                }
            }
        }
    }
    if (!top->hasFixedDownDistance()) {
        for (const MeasureBase* mb2 : bottom->measures()) {
            if (mb2->isMeasure()) {
                const Measure* m = toMeasure(mb2);
                const Spacer* sp = m->vspacerUp(firstStaff);
                if (sp) {
                    dist = std::max(dist, sp->absoluteGap());
                }
            }
        }

        dist = std::max(dist, sld);
    }
    return dist;
}

void SystemLayout::removeElementFromSkyline(EngravingItem* element, const System* system)
{
    Skyline& skyline = system->staff(element->staffIdx())->skyline();
    bool isAbove = element->isArticulationFamily() ? toArticulation(element)->up() : element->placeAbove();
    SkylineLine& skylineLine = isAbove ? skyline.north() : skyline.south();

    skylineLine.remove_if([element](ShapeElement& shapeEl) {
        return shapeEl.item() && (element == shapeEl.item() || element == shapeEl.item()->parentItem());
    });
}

void SystemLayout::updateSkylineForElement(EngravingItem* element, const System* system, double yMove)
{
    Skyline& skyline = system->staff(element->staffIdx())->skyline();
    bool isAbove = element->isArticulationFamily() ? toArticulation(element)->up() : element->placeAbove();
    SkylineLine& skylineLine = isAbove ? skyline.north() : skyline.south();
    for (ShapeElement& shapeEl : skylineLine.elements()) {
        const EngravingItem* itemInSkyline = shapeEl.item();
        if (itemInSkyline && itemInSkyline->isText() && itemInSkyline->explicitParent() && itemInSkyline->parent()->isSLineSegment()) {
            itemInSkyline = itemInSkyline->parentItem();
        }
        if (itemInSkyline == element) {
            shapeEl.translate(0.0, yMove);
        }
    }
}

void SystemLayout::centerElementsBetweenStaves(const System* system)
{
    std::vector<EngravingItem*> centeredItems;

    for (SpannerSegment* spannerSeg : system->spannerSegments()) {
        if (spannerSeg->isHairpinSegment() && elementShouldBeCenteredBetweenStaves(spannerSeg, system)) {
            centerElementBetweenStaves(spannerSeg, system);
            centeredItems.push_back(spannerSeg);
        } else if (spannerSeg->isWhammyBarSegment() && whammyBarShouldBeCenteredBetweenStaves(toWhammyBarSegment(spannerSeg), system)) {
            centerElementBetweenStaves(spannerSeg, system);
            centeredItems.push_back(spannerSeg);
        }
    }

    for (const MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (const Segment& seg : toMeasure(mb)->segments()) {
            for (EngravingItem* item : seg.elist()) {
                if (item && item->isMMRest() && mmRestShouldBeCenteredBetweenStaves(toMMRest(item), system)) {
                    centerMMRestBetweenStaves(toMMRest(item), system);
                }
            }
            for (EngravingItem* item : seg.annotations()) {
                if ((item->isDynamic() || item->isExpression()) && elementShouldBeCenteredBetweenStaves(item, system)) {
                    centerElementBetweenStaves(item, system);
                    centeredItems.push_back(item);
                }
            }
        }
    }

    AlignmentLayout::alignStaffCenteredItems(centeredItems, system);
}

void SystemLayout::centerBigTimeSigsAcrossStaves(const System* system)
{
    staff_idx_t nstaves = system->score()->nstaves();
    for (MeasureBase* mb : system->measures()) {
        if (!mb->isMeasure()) {
            continue;
        }
        for (Segment& segment : toMeasure(mb)->segments()) {
            if (!segment.isType(SegmentType::TimeSigType)) {
                continue;
            }
            for (staff_idx_t staffIdx = 0; staffIdx < nstaves; ++staffIdx) {
                TimeSig* timeSig = toTimeSig(segment.element(staff2track(staffIdx)));
                if (!timeSig || !timeSig->showOnThisStaff()) {
                    continue;
                }
                staff_idx_t thisStaffIdx = timeSig->effectiveStaffIdx();
                if (thisStaffIdx == muse::nidx) {
                    continue;
                }
                staff_idx_t nextStaffIdx = thisStaffIdx;
                for (staff_idx_t idx = thisStaffIdx + 1; idx < nstaves; ++idx) {
                    TimeSig* nextTimeSig = toTimeSig(segment.element(staff2track(idx)));
                    if (nextTimeSig && nextTimeSig->showOnThisStaff()) {
                        staff_idx_t nextTimeSigStave = nextTimeSig->effectiveStaffIdx();
                        if (nextTimeSigStave != muse::nidx) {
                            nextStaffIdx = system->prevVisibleStaff(nextTimeSigStave);
                            break;
                        }
                    }
                    if (idx == nstaves - 1) {
                        nextStaffIdx = system->prevVisibleStaff(nstaves);
                        break;
                    }
                }
                double yTop = system->staff(thisStaffIdx)->y() + system->score()->staff(thisStaffIdx)->staffHeight(segment.tick());
                double yBottom = system->staff(nextStaffIdx)->y() + system->score()->staff(nextStaffIdx)->staffHeight(segment.tick());
                double newYPos = 0.5 * (yBottom - yTop);
                timeSig->mutldata()->setPosY(newYPos);
            }
        }
    }
}

bool SystemLayout::elementShouldBeCenteredBetweenStaves(const EngravingItem* item, const System* system)
{
    if (item->offset().y() != item->propertyDefault(Pid::OFFSET).value<PointF>().y()) {
        // NOTE: because of current limitations of the offset system, we can't center an element that's been manually moved.
        return false;
    }

    const Part* itemPart = item->part();
    IF_ASSERT_FAILED(itemPart) {
        return false;
    }

    bool centerStyle = item->style().styleB(Sid::dynamicsHairpinsAutoCenterOnGrandStaff);
    AutoOnOff centerProperty = item->getProperty(Pid::CENTER_BETWEEN_STAVES).value<AutoOnOff>();
    if (itemPart->nstaves() <= 1 || centerProperty == AutoOnOff::OFF || (!centerStyle && centerProperty != AutoOnOff::ON)) {
        return false;
    }

    if (centerProperty != AutoOnOff::ON && !itemPart->instrument()->isNormallyMultiStaveInstrument()) {
        return false;
    }

    const Staff* thisStaff = item->staff();
    const std::vector<Staff*>& partStaves = itemPart->staves();
    IF_ASSERT_FAILED(partStaves.size() > 0) {
        return false;
    }

    if ((thisStaff == partStaves.front() && item->placeAbove()) || (thisStaff == partStaves.back() && item->placeBelow())) {
        return false;
    }

    staff_idx_t thisIdx = thisStaff->idx();
    if (item->placeAbove()) {
        IF_ASSERT_FAILED(thisIdx > 0) {
            return false;
        }
    }

    staff_idx_t nextIdx = item->placeAbove() ? system->prevVisibleStaff(thisIdx) : system->nextVisibleStaff(thisIdx);
    if (nextIdx == muse::nidx || !muse::contains(partStaves, item->score()->staff(nextIdx))) {
        return false;
    }

    return centerProperty == AutoOnOff::ON || item->appliesToAllVoicesInInstrument();
}

bool SystemLayout::mmRestShouldBeCenteredBetweenStaves(const MMRest* mmRest, const System* system)
{
    if (!mmRest->style().styleB(Sid::mmRestBetweenStaves)) {
        return false;
    }

    const Part* itemPart = mmRest->part();
    if (itemPart->nstaves() <= 1) {
        return false;
    }

    staff_idx_t thisStaffIdx = mmRest->staffIdx();
    staff_idx_t prevStaffIdx = system->prevVisibleStaff(thisStaffIdx);

    return prevStaffIdx != muse::nidx && mmRest->score()->staff(prevStaffIdx)->part() == itemPart;
}

bool SystemLayout::whammyBarShouldBeCenteredBetweenStaves(const WhammyBarSegment* wbar, const System* system)
{
    if (wbar->offset().y() != wbar->propertyDefault(Pid::OFFSET).value<PointF>().y()) {
        // NOTE: because of current limitations of the offset system, we can't center an element that's been manually moved.
        return false;
    }

    staff_idx_t staffIdx = wbar->staffIdx();
    Staff* thisStaff = wbar->staff();
    Staff* nextStaff = wbar->score()->staff(staffIdx + 1);
    bool nextIsLinkedTab = nextStaff && nextStaff->isTabStaff(wbar->tick()) && thisStaff->isLinked(nextStaff);
    SysStaff* nextSysStaff = system->staff(staffIdx + 1);
    return wbar->placeBelow() && nextIsLinkedTab && nextSysStaff && nextSysStaff->show();
}

bool SystemLayout::elementHasAnotherStackedOutside(const EngravingItem* element, const Shape& elementShape, const SkylineLine& skylineLine)
{
    double elemShapeLeft = -elementShape.left();
    double elemShapeRight = elementShape.right();
    double elemShapeTop = elementShape.top();
    double elemShapeBottom = elementShape.bottom();

    for (const ShapeElement& skylineElement : skylineLine.elements()) {
        const EngravingItem* skylineItem = skylineElement.item();
        if (!skylineItem || skylineItem == element || skylineItem->parent() == element
            || Autoplace::itemsShouldIgnoreEachOther(element, skylineItem)) {
            continue;
        }
        bool intersectHorizontally = elemShapeRight > skylineElement.left() && elemShapeLeft < skylineElement.right();
        if (!intersectHorizontally) {
            continue;
        }
        bool skylineElementIsStackedOnIt = skylineLine.isNorth() ? skylineElement.top() < elemShapeTop
                                           : skylineElement.bottom() > elemShapeBottom;
        if (skylineElementIsStackedOnIt) {
            return true;
        }
    }

    return false;
}

void SystemLayout::centerElementBetweenStaves(EngravingItem* element, const System* system)
{
    bool isAbove = element->placeAbove();
    staff_idx_t thisIdx = element->staffIdx();
    if (isAbove) {
        IF_ASSERT_FAILED(thisIdx > 0) {
            return;
        }
    }
    staff_idx_t nextIdx = isAbove ? system->prevVisibleStaff(thisIdx) : system->nextVisibleStaff(thisIdx);
    IF_ASSERT_FAILED(nextIdx != muse::nidx) {
        return;
    }

    SysStaff* thisStaff = system->staff(thisIdx);
    SysStaff* nextStaff = system->staff(nextIdx);

    IF_ASSERT_FAILED(thisStaff && nextStaff) {
        return;
    }

    double elementXinSystemCoord = element->pageX() - system->pageX();
    const double minHorizontalClearance = system->style().styleMM(Sid::skylineMinHorizontalClearance);

    Shape elementShape = element->ldata()->shape()
                         .translated(PointF(elementXinSystemCoord, element->y()))
                         .adjust(-minHorizontalClearance, 0.0, minHorizontalClearance, 0.0);
    elementShape.remove_if([](ShapeElement& shEl) { return shEl.ignoreForLayout(); });

    const SkylineLine& skylineOfThisStaff = isAbove ? thisStaff->skyline().north() : thisStaff->skyline().south();

    if (elementHasAnotherStackedOutside(element, elementShape, skylineOfThisStaff)) {
        return;
    }

    SkylineLine thisSkyline = skylineOfThisStaff.getFilteredCopy([element](const ShapeElement& shEl) {
        const EngravingItem* shapeItem = shEl.item();
        if (!shapeItem) {
            return false;
        }
        return shapeItem->isAccidental() || Autoplace::itemsShouldIgnoreEachOther(element, shapeItem);
    });

    double yStaffDiff = nextStaff->y() - thisStaff->y();
    SkylineLine nextSkyline = isAbove ? nextStaff->skyline().south() : nextStaff->skyline().north();
    nextSkyline.translateY(yStaffDiff);

    double elementMinDist = element->minDistance().toMM(element->spatium());
    double availSpaceAbove = (isAbove ? nextSkyline.verticalClaranceBelow(elementShape) : thisSkyline.verticalClaranceBelow(elementShape))
                             - elementMinDist;
    double availSpaceBelow = (isAbove ? thisSkyline.verticalClearanceAbove(elementShape) : nextSkyline.verticalClearanceAbove(elementShape))
                             - elementMinDist;

    double yMove = 0.5 * (availSpaceBelow - availSpaceAbove);

    element->mutldata()->moveY(yMove);

    availSpaceAbove += yMove;
    availSpaceBelow -= yMove;
    element->mutldata()->setStaffCenteringInfo(std::max(availSpaceAbove, 0.0), std::max(availSpaceBelow, 0.0));

    updateSkylineForElement(element, system, yMove);
}

void SystemLayout::centerMMRestBetweenStaves(MMRest* mmRest, const System* system)
{
    staff_idx_t thisIdx = mmRest->staffIdx();
    IF_ASSERT_FAILED(thisIdx > 0) {
        return;
    }

    staff_idx_t prevIdx = system->prevVisibleStaff(thisIdx);
    IF_ASSERT_FAILED(prevIdx != muse::nidx) {
        return;
    }

    SysStaff* thisStaff = system->staff(thisIdx);
    SysStaff* prevStaff = system->staff(prevIdx);
    double prevStaffHeight = system->score()->staff(prevIdx)->staffHeight(mmRest->tick());
    double yStaffDiff = prevStaff->y() + prevStaffHeight - thisStaff->y();

    PointF mmRestDefaultNumberPosition = mmRest->numberPos() - PointF(0.0, mmRest->numberOffset().toMM(mmRest->spatium()));
    RectF numberBbox = mmRest->numberRect().translated(mmRestDefaultNumberPosition + mmRest->pos());
    double yBaseLine = 0.5 * (yStaffDiff - numberBbox.height());
    double yDiff = yBaseLine - numberBbox.top();

    mmRest->mutldata()->yNumberPos += yDiff;
}
