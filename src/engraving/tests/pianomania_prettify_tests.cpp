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

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <vector>

#include <QFontDatabase>

#include "engraving/dom/accidental.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/barline.h"
#include "engraving/dom/bracketItem.h"
#include "engraving/dom/chord.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/expression.h"
#include "engraving/dom/fermata.h"
#include "engraving/dom/factory.h"
#include "engraving/editing/editdata.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/beam.h"
#include "engraving/dom/fingering.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/hook.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/ottava.h"
#include "engraving/dom/page.h"
#include "engraving/dom/part.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/spanner.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stem.h"
#include "engraving/dom/stafftext.h"
#include "engraving/dom/system.h"
#include "engraving/dom/tempo.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/text.h"
#include "engraving/dom/textlinebase.h"
#include "engraving/dom/articulation.h"
#include "engraving/dom/tuplet.h"
#include "engraving/editing/undo.h"
#include "engraving/pm/pmlayout.h"
#include "engraving/pm/pmprettify.h"
#include "engraving/infrastructure/mscwriter.h"
#include "engraving/rw/mscsaver.h"
#include "engraving/types/types.h"
#include "io/file.h"

#include "engraving/rendering/score/slurtielayout.h"
#include "draw/types/transform.h"

#include "utils/scorerw.h"

using namespace mu::engraving;

namespace {

constexpr std::array<Grip, 4> PRETTIFY_GRIPS = {
    Grip::START, Grip::BEZIER1, Grip::BEZIER2, Grip::END
};

constexpr std::array<Pid, 4> PRETTIFY_SLUR_PROPERTIES = {
    Pid::SLUR_UOFF1, Pid::SLUR_UOFF2, Pid::SLUR_UOFF3, Pid::SLUR_UOFF4
};

class ApplicationFontRegistration
{
public:
    explicit ApplicationFontRegistration(const String& path)
        : m_id(QFontDatabase::addApplicationFont(QString::fromStdString(path.toStdString())))
    {
    }

    ~ApplicationFontRegistration()
    {
        if (m_id >= 0) {
            QFontDatabase::removeApplicationFont(m_id);
        }
    }

    bool valid() const { return m_id >= 0; }

private:
    int m_id = -1;
};

struct SlurSnapshotEntry
{
    Slur* slur = nullptr;
    size_t segmentIndex = 0;
    std::array<PointF, 4> offsets;
    std::array<PropertyFlags, 4> offsetFlags;
    PointF offset;
    PropertyFlags offsetFlagsGeneral = PropertyFlags::STYLED;
    bool autoplace = true;
    PropertyFlags autoplaceFlags = PropertyFlags::STYLED;
    PointF endPointOff1;
    PointF endPointOff2;
    double extraHeight = 0.0;
    OffsetChange offsetChanged = OffsetChange::NONE;
    PointF changedPos;
    double spatium = 1.0;
};

struct FingeringSnapshotEntry
{
    Fingering* fingering = nullptr;
    PointF offset;
    PropertyFlags offsetFlags = PropertyFlags::STYLED;
    double minDistance = 0.0;
    PropertyFlags minDistanceFlags = PropertyFlags::STYLED;
    PlacementV placement = PlacementV::ABOVE;
    PropertyFlags placementFlags = PropertyFlags::STYLED;
    bool autoplace = true;
    PropertyFlags autoplaceFlags = PropertyFlags::STYLED;
    OffsetChange offsetChanged = OffsetChange::NONE;
    PointF changedPos;
    double spatium = 1.0;
};

struct PrettifySnapshot
{
    std::vector<SlurSnapshotEntry> slurSegments;
    std::vector<FingeringSnapshotEntry> fingerings;
};

using StructuralAssignment = std::vector<std::pair<int, int> >;

struct TempoSnapshotEntry
{
    String xmlText;
    String plainText;
    bool followText = false;
    bool visible = true;
};

struct StaffTextStyleSnapshotEntry
{
    String plainText;
    TextStyleType textStyleType = TextStyleType::DEFAULT;
    PropertyFlags textStyleFlags = PropertyFlags::STYLED;
    PropertyFlags fontFaceFlags = PropertyFlags::STYLED;
    PropertyFlags fontStyleFlags = PropertyFlags::STYLED;
    PropertyFlags fontSizeFlags = PropertyFlags::STYLED;
};

void collectFingerings(void* data, EngravingItem* item)
{
    if (!item || !item->isFingering()) {
        return;
    }

    auto* snapshot = static_cast<PrettifySnapshot*>(data);
    Fingering* fingering = toFingering(item);
    snapshot->fingerings.push_back(FingeringSnapshotEntry {
        fingering,
        fingering->offset(),
        fingering->propertyFlags(Pid::OFFSET),
        fingering->minDistance().val(),
        fingering->propertyFlags(Pid::MIN_DISTANCE),
        fingering->placement(),
        fingering->propertyFlags(Pid::PLACEMENT),
        fingering->autoplace(),
        fingering->propertyFlags(Pid::AUTOPLACE),
        fingering->ldata()->offsetChanged(),
        fingering->ldata()->autoplace.changedPos,
        std::max(1.0, fingering->spatium())
    });
}

PrettifySnapshot capturePrettifySnapshot(Score* score)
{
    PrettifySnapshot snapshot;
    if (!score) {
        return snapshot;
    }

    for (const auto& pair : score->spanner()) {
        Spanner* spanner = pair.second;
        if (!spanner || !spanner->isSlur()) {
            continue;
        }

        Slur* slur = toSlur(spanner);
        for (size_t i = 0; i < slur->nsegments(); ++i) {
            SlurSegment* segment = slur->segmentAt(static_cast<int>(i));
            if (!segment) {
                continue;
            }

            SlurSnapshotEntry entry;
            entry.slur = slur;
            entry.segmentIndex = i;
            for (size_t gripIndex = 0; gripIndex < PRETTIFY_GRIPS.size(); ++gripIndex) {
                const Grip grip = PRETTIFY_GRIPS[gripIndex];
                const Pid property = PRETTIFY_SLUR_PROPERTIES[gripIndex];
                entry.offsets[gripIndex] = segment->ups(grip).off;
                entry.offsetFlags[gripIndex] = segment->propertyFlags(property);
            }
            entry.offset = segment->offset();
            entry.offsetFlagsGeneral = segment->propertyFlags(Pid::OFFSET);
            entry.autoplace = segment->autoplace();
            entry.autoplaceFlags = segment->propertyFlags(Pid::AUTOPLACE);
            entry.endPointOff1 = segment->endPointOff1();
            entry.endPointOff2 = segment->endPointOff2();
            entry.extraHeight = segment->extraHeight();
            entry.offsetChanged = segment->ldata()->offsetChanged();
            entry.changedPos = segment->ldata()->autoplace.changedPos;
            entry.spatium = std::max(1.0, segment->spatium());
            snapshot.slurSegments.push_back(entry);
        }
    }

    score->scanElements([&snapshot](mu::engraving::EngravingItem* item) { collectFingerings(&snapshot, item); });
    return snapshot;
}

StructuralAssignment captureStructuralAssignment(Score* score)
{
    StructuralAssignment assignment;
    const Page* currentPage = nullptr;
    int pageIndex = -1;
    for (const System* system : score->systems()) {
        const Measure* firstMeasure = system ? system->firstMeasure() : nullptr;
        if (!firstMeasure) {
            continue;
        }
        const Page* page = system->page();
        if (page != currentPage) {
            currentPage = page;
            ++pageIndex;
        }
        assignment.emplace_back(pageIndex, firstMeasure->tick().ticks());
    }
    return assignment;
}

bool pointNear(const PointF& a, const PointF& b, double tolerance)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y()) <= tolerance;
}

double staffYInSystem(const System* system, staff_idx_t staffIdx)
{
    if (!system || staffIdx == muse::nidx || staffIdx >= system->staves().size()) {
        return 0.0;
    }

    return system->staff(staffIdx)->y();
}

RectF fingeringSystemRect(const Fingering* fingering)
{
    const Note* note = fingering ? fingering->note() : nullptr;
    const Chord* chord = note ? note->chord() : nullptr;
    const Segment* segment = chord ? chord->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    if (!fingering || !note || !chord || !segment || !measure || !fingering->ldata()) {
        return RectF();
    }

    return fingering->ldata()->bbox().translated(PointF(0.0, staffYInSystem(measure->system(), chord->vStaffIdx()))
                                                 + fingering->pos() + note->pos() + chord->pos() + segment->pos()
                                                 + measure->pos());
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

double fingeringNoteheadDistance(const Fingering* fingering)
{
    const RectF fingeringRect = fingeringSystemRect(fingering);
    const RectF noteRect = noteSystemRect(fingering ? fingering->note() : nullptr);
    if (fingeringRect.isNull() || noteRect.isNull()) {
        return 0.0;
    }

    const bool above = fingering->placement() == PlacementV::ABOVE;
    const double distance = above ? noteRect.top() - fingeringRect.bottom()
                            : fingeringRect.top() - noteRect.bottom();
    return std::max(0.0, distance);
}

RectF restSystemRect(const Rest* rest)
{
    const Segment* segment = rest ? rest->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    if (!rest || !segment || !measure) {
        return RectF();
    }

    return rest->shape().bbox().translated(PointF(0.0, staffYInSystem(measure->system(), rest->vStaffIdx()))
                                           + rest->pos() + segment->pos() + measure->pos() + rest->staffOffset());
}

RectF hairpinSegmentSystemRect(const SpannerSegment* segment)
{
    if (!segment || !segment->ldata()) {
        return RectF();
    }

    return segment->ldata()->bbox().translated(segment->pos()
                                               + PointF(0.0, staffYInSystem(segment->system(), segment->vStaffIdx())));
}

std::vector<HairpinSegment*> collectLineHairpinSegments(Score* score)
{
    std::vector<HairpinSegment*> segments;
    if (!score) {
        return segments;
    }

    for (const auto& pair : score->spanner()) {
        Spanner* spanner = pair.second;
        if (!spanner || !spanner->isHairpin()) {
            continue;
        }

        Hairpin* hairpin = toHairpin(spanner);
        if (!hairpin->isLineType()) {
            continue;
        }
        for (size_t i = 0; i < hairpin->nsegments(); ++i) {
            HairpinSegment* segment = toHairpinSegment(hairpin->segmentAt(static_cast<int>(i)));
            if (segment) {
                segments.push_back(segment);
            }
        }
    }

    return segments;
}

std::vector<RectF> collectChordAndFingeringRects(Score* score, staff_idx_t staffIdx)
{
    std::vector<RectF> rects;
    std::set<const Beam*> seenBeams;
    if (!score) {
        return rects;
    }

    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        System* system = measure->system();
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            if (!segment->isChordRestType()) {
                continue;
            }
            for (EngravingItem* item : segment->elist()) {
                if (!item || !item->isChord() || item->vStaffIdx() != staffIdx) {
                    continue;
                }

                Chord* chord = toChord(item);
                for (Note* note : chord->notes()) {
                    const RectF noteRect = noteSystemRect(note);
                    if (!noteRect.isNull()) {
                        rects.push_back(noteRect);
                    }
                    for (EngravingItem* noteItem : note->el()) {
                        if (noteItem && noteItem->isFingering()) {
                            const RectF fingeringRect = fingeringSystemRect(toFingering(noteItem));
                            if (!fingeringRect.isNull()) {
                                rects.push_back(fingeringRect);
                            }
                        }
                    }
                }

                const Stem* stem = chord->stem();
                if (stem && stem->visible() && stem->ldata() && !stem->ldata()->isSkipDraw()) {
                    const RectF stemRect = stem->ldata()->bbox().translated(
                        PointF(0.0, staffYInSystem(system, staffIdx))
                        + stem->pos() + chord->pos() + segment->pos() + measure->pos());
                    if (!stemRect.isNull()) {
                        rects.push_back(stemRect);
                    }
                }

                const Beam* beam = chord->beam();
                if (beam && beam->visible() && beam->ldata() && !beam->ldata()->isSkipDraw() && seenBeams.insert(beam).second) {
                    const RectF beamRect = beam->ldata()->bbox();
                    if (!beamRect.isNull()) {
                        rects.push_back(beamRect);
                    }
                }
            }
        }
    }

    return rects;
}

std::vector<RectF> fingeringOwnerStructuralRects(const Fingering* fingering)
{
    std::vector<RectF> rects;
    const Note* note = fingering ? fingering->note() : nullptr;
    const Chord* chord = note ? note->chord() : nullptr;
    const Segment* segment = chord ? chord->segment() : nullptr;
    const Measure* measure = segment ? segment->measure() : nullptr;
    const System* system = measure ? measure->system() : nullptr;
    if (!chord || !segment || !measure || !system) {
        return rects;
    }

    const PointF chordOrigin = PointF(0.0, staffYInSystem(system, chord->vStaffIdx()))
                               + chord->pos() + segment->pos() + measure->pos();
    const Stem* stem = chord->stem();
    if (stem && stem->visible() && stem->ldata() && !stem->ldata()->isSkipDraw()) {
        rects.push_back(stem->ldata()->bbox().translated(chordOrigin + stem->pos() + stem->staffOffset()));
    }

    const Hook* hook = chord->hook();
    if (hook && hook->visible() && hook->ldata() && !hook->ldata()->isSkipDraw()) {
        rects.push_back(hook->ldata()->bbox().translated(chordOrigin + hook->pos() + hook->staffOffset()));
    }

    const Beam* beam = chord->beam();
    if (beam && beam->visible() && beam->ldata() && !beam->ldata()->isSkipDraw()) {
        const PointF beamOrigin = beam->pagePos() - system->pagePos();
        for (const BeamSegment* beamSegment : beam->beamSegments()) {
            const Shape beamSegmentShape = beamSegment->shape();
            for (const ShapeElement& box : beamSegmentShape.elements()) {
                rects.push_back(box.translated(beamOrigin));
            }
        }
    }

    return rects;
}

double minimumShapeDistance(const Shape& first, const Shape& second)
{
    double minimum = std::numeric_limits<double>::max();
    for (const RectF& a : first.toRects()) {
        for (const RectF& b : second.toRects()) {
            const double horizontal = std::max({ a.left() - b.right(), b.left() - a.right(), 0.0 });
            const double vertical = std::max({ a.top() - b.bottom(), b.top() - a.bottom(), 0.0 });
            minimum = std::min(minimum, std::hypot(horizontal, vertical));
        }
    }
    return minimum;
}

Shape sampledPathShape(const PainterPath& path, const PointF& offset)
{
    Shape shape(Shape::Type::Composite);
    PointF current;
    for (size_t i = 0; i < path.elementCount(); ++i) {
        const PainterPath::Element element = path.elementAt(i);
        if (element.isMoveTo()) {
            current = PointF(element.x, element.y) + offset;
            continue;
        }
        if (!element.isCurveTo() || i + 2 >= path.elementCount()) {
            continue;
        }

        const PointF p0 = current;
        const PointF p1 = PointF(element.x, element.y) + offset;
        const PainterPath::Element control2 = path.elementAt(i + 1);
        const PainterPath::Element end = path.elementAt(i + 2);
        const PointF p2 = PointF(control2.x, control2.y) + offset;
        const PointF p3 = PointF(end.x, end.y) + offset;
        PointF previous = p0;
        for (int step = 1; step <= 64; ++step) {
            const double t = double(step) / 64.0;
            const double u = 1.0 - t;
            const PointF point = p0 * (u * u * u) + p1 * (3.0 * u * u * t)
                                 + p2 * (3.0 * u * t * t) + p3 * (t * t * t);
            RectF bounds(previous, point);
            bounds = bounds.normalized();
            bounds.adjust(-1e-6, -1e-6, 1e-6, 1e-6);
            shape.add(bounds);
            previous = point;
        }
        current = p3;
        i += 2;
    }
    return shape;
}

RectF tupletNumberSystemRect(const Tuplet* tuplet)
{
    const Text* number = tuplet ? tuplet->number() : nullptr;
    if (!number) {
        return RectF();
    }

    return number->pageBoundingRect();
}

bool rectsOverlap(const RectF& a, const RectF& b)
{
    return !a.isNull() && !b.isNull()
           && a.left() < b.right() && a.right() > b.left()
           && a.top() < b.bottom() && a.bottom() > b.top();
}

bool slurSnapshotsEquivalent(const std::vector<SlurSnapshotEntry>& a, const std::vector<SlurSnapshotEntry>& b)
{
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        const SlurSnapshotEntry& left = a[i];
        const SlurSnapshotEntry& right = b[i];
        const double tolerance = 0.02 * left.spatium;
        if (left.slur != right.slur
            || left.segmentIndex != right.segmentIndex
            || left.offsetFlagsGeneral != right.offsetFlagsGeneral
            || left.autoplace != right.autoplace
            || left.autoplaceFlags != right.autoplaceFlags
            || left.offsetChanged != right.offsetChanged
            || std::abs(left.extraHeight - right.extraHeight) > 0.02
            || !pointNear(left.offset, right.offset, tolerance)) {
            return false;
        }
        if (!pointNear(left.endPointOff1, right.endPointOff1, tolerance)
            || !pointNear(left.endPointOff2, right.endPointOff2, tolerance)
            || !pointNear(left.changedPos, right.changedPos, tolerance)) {
            return false;
        }
        for (size_t gripIndex = 0; gripIndex < PRETTIFY_GRIPS.size(); ++gripIndex) {
            if (left.offsetFlags[gripIndex] != right.offsetFlags[gripIndex]
                || !pointNear(left.offsets[gripIndex], right.offsets[gripIndex], tolerance)) {
                return false;
            }
        }
    }

    return true;
}

bool fingeringSnapshotsEquivalent(const std::vector<FingeringSnapshotEntry>& a, const std::vector<FingeringSnapshotEntry>& b)
{
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        const FingeringSnapshotEntry& left = a[i];
        const FingeringSnapshotEntry& right = b[i];
        const double tolerance = 0.02 * left.spatium;
        if (left.fingering != right.fingering
            || left.offsetFlags != right.offsetFlags
            || left.minDistanceFlags != right.minDistanceFlags
            || left.placement != right.placement
            || left.placementFlags != right.placementFlags
            || left.autoplace != right.autoplace
            || left.autoplaceFlags != right.autoplaceFlags
            || left.offsetChanged != right.offsetChanged
            || !pointNear(left.offset, right.offset, tolerance)
            || !pointNear(left.changedPos, right.changedPos, tolerance)
            || std::abs(left.minDistance - right.minDistance) > 0.02) {
            return false;
        }
    }

    return true;
}

bool snapshotsEquivalent(const PrettifySnapshot& a, const PrettifySnapshot& b)
{
    return slurSnapshotsEquivalent(a.slurSegments, b.slurSegments)
           && fingeringSnapshotsEquivalent(a.fingerings, b.fingerings);
}

std::vector<Fingering*> collectFingeringsByText(Score* score, const String& text)
{
    std::vector<Fingering*> fingerings;
    const PrettifySnapshot snapshot = capturePrettifySnapshot(score);
    for (const FingeringSnapshotEntry& entry : snapshot.fingerings) {
        if (entry.fingering && entry.fingering->plainText() == text) {
            fingerings.push_back(entry.fingering);
        }
    }
    return fingerings;
}

std::vector<Tuplet*> collectTuplets(Score* score)
{
    std::vector<Tuplet*> tuplets;
    std::set<Tuplet*> seen;
    if (!score) {
        return tuplets;
    }

    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            if (!segment->isChordRestType()) {
                continue;
            }
            for (EngravingItem* item : segment->elist()) {
                if (!item || !item->isChordRest()) {
                    continue;
                }
                for (Tuplet* tuplet = toChordRest(item)->tuplet(); tuplet; tuplet = tuplet->tuplet()) {
                    if (seen.insert(tuplet).second) {
                        tuplets.push_back(tuplet);
                    }
                }
            }
        }
    }
    return tuplets;
}

std::vector<Rest*> collectVisibleRests(Score* score)
{
    std::vector<Rest*> rests;
    if (!score) {
        return rests;
    }

    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            if (!segment->isChordRestType()) {
                continue;
            }
            for (EngravingItem* item : segment->elist()) {
                if (item && item->isRest() && item->visible() && !toRest(item)->isGap()) {
                    rests.push_back(toRest(item));
                }
            }
        }
    }
    return rests;
}

size_t countBraceBracketsSpanning(const Score* score, staff_idx_t startStaffIdx, size_t span)
{
    if (!score) {
        return 0;
    }

    size_t count = 0;
    for (const Staff* staff : score->staves()) {
        if (!staff) {
            continue;
        }

        for (const BracketItem* bracket : staff->brackets()) {
            if (bracket
                && staff->idx() == startStaffIdx
                && bracket->bracketType() == BracketType::BRACE
                && bracket->bracketSpan() == span) {
                ++count;
            }
        }
    }

    return count;
}

std::vector<TempoText*> collectTempoTexts(Score* score)
{
    std::vector<TempoText*> tempoTexts;
    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            for (EngravingItem* item : segment->annotations()) {
                if (item && item->isTempoText()) {
                    tempoTexts.push_back(toTempoText(item));
                }
            }
        }
    }
    return tempoTexts;
}

std::vector<StaffText*> collectStaffTexts(Score* score)
{
    std::vector<StaffText*> staffTexts;
    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            for (EngravingItem* item : segment->annotations()) {
                if (item && item->isStaffText()) {
                    staffTexts.push_back(toStaffText(item));
                }
            }
        }
    }
    return staffTexts;
}

std::vector<TempoSnapshotEntry> captureTempoSnapshot(Score* score)
{
    std::vector<TempoSnapshotEntry> snapshot;
    for (TempoText* tempoText : collectTempoTexts(score)) {
        snapshot.push_back(TempoSnapshotEntry {
            tempoText->xmlText(),
            tempoText->plainText(),
            tempoText->followText(),
            tempoText->visible()
        });
    }
    return snapshot;
}

bool tempoSnapshotsEquivalent(const std::vector<TempoSnapshotEntry>& a, const std::vector<TempoSnapshotEntry>& b)
{
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].xmlText != b[i].xmlText
            || a[i].plainText != b[i].plainText
            || a[i].followText != b[i].followText
            || a[i].visible != b[i].visible) {
            return false;
        }
    }

    return true;
}

std::vector<StaffTextStyleSnapshotEntry> captureStaffTextStyleSnapshot(Score* score)
{
    std::vector<StaffTextStyleSnapshotEntry> snapshot;
    for (StaffText* staffText : collectStaffTexts(score)) {
        snapshot.push_back(StaffTextStyleSnapshotEntry {
            staffText->plainText(),
            staffText->textStyleType(),
            staffText->propertyFlags(Pid::TEXT_STYLE),
            staffText->propertyFlags(Pid::FONT_FACE),
            staffText->propertyFlags(Pid::FONT_STYLE),
            staffText->propertyFlags(Pid::FONT_SIZE)
        });
    }
    return snapshot;
}

bool staffTextStyleSnapshotsEquivalent(const std::vector<StaffTextStyleSnapshotEntry>& a,
                                       const std::vector<StaffTextStyleSnapshotEntry>& b)
{
    if (a.size() != b.size()) {
        return false;
    }

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].plainText != b[i].plainText
            || a[i].textStyleType != b[i].textStyleType
            || a[i].textStyleFlags != b[i].textStyleFlags
            || a[i].fontFaceFlags != b[i].fontFaceFlags
            || a[i].fontStyleFlags != b[i].fontStyleFlags
            || a[i].fontSizeFlags != b[i].fontSizeFlags) {
            return false;
        }
    }

    return true;
}

SlurSegment* firstSlurSegment(Score* score)
{
    if (!score) {
        return nullptr;
    }

    for (const auto& pair : score->spanner()) {
        Spanner* spanner = pair.second;
        if (!spanner || !spanner->isSlur()) {
            continue;
        }

        Slur* slur = toSlur(spanner);
        if (slur->nsegments() == 0) {
            continue;
        }

        return slur->segmentAt(0);
    }

    return nullptr;
}

void relayoutScore(Score* score)
{
    score->setLayoutAll();
    score->doLayout();
}

mu::engraving::pm::PmPrettifyResult applyPrettifyCommand(
    Score* score, const mu::engraving::pm::PmPrettifyOptions& options = {})
{
    score->startCmd(TranslatableString::untranslatable("Pianomania prettify test"));
    const mu::engraving::pm::PmPrettifyResult result = mu::engraving::pm::applyPianomaniaPrettify(score, options);
    score->endCmd(!result.changed || result.structuralAssignmentChanged);
    relayoutScore(score);
    return result;
}

template<typename T>
std::vector<T*> collectAnnotations(Score* score, bool (EngravingObject::*isType)() const)
{
    std::vector<T*> items;
    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            for (EngravingItem* item : segment->annotations()) {
                if (item && (item->*isType)()) {
                    items.push_back(static_cast<T*>(item));
                }
            }
        }
    }
    return items;
}

Expression* expressionWithText(Score* score, const String& text)
{
    for (Expression* expression : collectAnnotations<Expression>(score, &EngravingObject::isExpression)) {
        if (expression->plainText() == text) {
            return expression;
        }
    }
    return nullptr;
}

EngravingItem* adjacentBarline(const Expression* expression, bool following)
{
    if (!expression || !expression->segment() || !expression->segment()->measure()->system()) {
        return nullptr;
    }

    const System* system = expression->segment()->measure()->system();
    for (Segment* segment = expression->segment(); segment && segment->measure()->system() == system;
         segment = following ? segment->next1enabled() : segment->prev1enabled()) {
        if (segment->segmentType() & SegmentType::BarLineType) {
            return segment->element(expression->staffIdx() * VOICES);
        }
    }
    return nullptr;
}

double followingBarlineClearance(const Expression* expression)
{
    const EngravingItem* barline = adjacentBarline(expression, true);
    return barline ? barline->pageBoundingRect().left() - expression->pageBoundingRect().right()
                   : -std::numeric_limits<double>::infinity();
}

double precedingBarlineClearance(const Expression* expression)
{
    const EngravingItem* barline = adjacentBarline(expression, false);
    return barline ? expression->pageBoundingRect().left() - barline->pageBoundingRect().right()
                   : -std::numeric_limits<double>::infinity();
}

std::vector<Hairpin*> collectHairpins(Score* score)
{
    std::vector<Hairpin*> hairpins;
    for (const auto& pair : score->spanner()) {
        if (pair.second && pair.second->isHairpin()) {
            hairpins.push_back(toHairpin(pair.second));
        }
    }
    return hairpins;
}

Hairpin* hairpinWithBeginText(Score* score)
{
    for (Hairpin* hairpin : collectHairpins(score)) {
        if (!hairpin->beginText().isEmpty()) {
            return hairpin;
        }
    }
    return nullptr;
}

Measure* measureAt(Score* score, int index)
{
    Measure* measure = score->firstMeasure();
    for (int i = 0; measure && i < index; ++i) {
        measure = measure->nextMeasure();
    }
    return measure;
}

} // namespace

class Engraving_PianomaniaPrettifyTests : public ::testing::Test
{
};

// Test value: Detects a slur crossing the upper accidental of an authored turn
// when that accidental is stored as staff text on a time-tick segment.
TEST_F(Engraving_PianomaniaPrettifyTests, slurClearsOrnamentAccidentalStaffText)
{
    const String freeSansPath = ScoreRW::rootPath() + u"/../../../fonts/FreeSans.ttf";
    const ApplicationFontRegistration freeSans(freeSansPath);
    ASSERT_TRUE(freeSans.valid()) << freeSansPath.toStdString();

    for (const String& symbol : { String(u"\u266d"), String(u"\u266e"), String(u"\u266f") }) {
        SCOPED_TRACE(symbol.toStdString());
        MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/ornament-accidental-slur.mscx");
        ASSERT_TRUE(score);
        StaffText* accidental = nullptr;
        for (StaffText* text : collectStaffTexts(score)) {
            if (text->plainText() == u"\u266d") {
                accidental = text;
                break;
            }
        }
        ASSERT_TRUE(accidental);
        accidental->setXmlText(symbol);
        const StructuralAssignment originalStructure = captureStructuralAssignment(score);
        mu::engraving::pm::applyPianomaniaAutoLayout(score);
        const Segment* anchor = toSegment(accidental->parentItem());
        const Measure* measure = anchor->measure();
        const System* system = measure->system();
        ASSERT_TRUE(system);
        SlurSegment* phrase = nullptr;
        for (const auto& pair : score->spanner()) {
            Spanner* spanner = pair.second;
            if (!spanner || !spanner->isSlur()) {
                continue;
            }
            Slur* slur = toSlur(spanner);
            if (slur->up() && slur->staffIdx() == accidental->staffIdx()
                && slur->tick() < accidental->tick() && slur->tick2() > accidental->tick()) {
                for (SpannerSegment* segment : slur->spannerSegments()) {
                    if (segment->system() == system) {
                        phrase = toSlurSegment(segment);
                        break;
                    }
                }
            }
        }
        ASSERT_TRUE(phrase);
        Slur* phraseSlur = phrase->slur();
        const auto curvePoints = [](const System* curveSystem, const SlurSegment* curveSegment) {
            std::array<PointF, 4> points;
            for (size_t i = 0; i < PRETTIFY_GRIPS.size(); ++i) {
                points[i] = curveSegment->ups(PRETTIFY_GRIPS[i]).pos() + curveSegment->pos()
                            + PointF(0.0, curveSystem->staff(curveSegment->vStaffIdx())->y());
            }
            return points;
        };
        const auto minimumClearanceFor = [](const std::array<PointF, 4>& points, const RectF& rect) {
            double minimum = std::numeric_limits<double>::infinity();
            for (int i = 0; i <= 2000; ++i) {
                const double t = static_cast<double>(i) / 2000.0;
                const double u = 1.0 - t;
                const PointF point = points[0] * (u * u * u) + points[1] * (3.0 * u * u * t)
                                     + points[2] * (3.0 * u * t * t) + points[3] * (t * t * t);
                if (point.x() >= rect.left() && point.x() <= rect.right()) {
                    minimum = std::min(minimum, rect.top() - point.y());
                }
            }
            return minimum;
        };
        const RectF initialAccidentalRect = accidental->ldata()->bbox().translated(
            accidental->pos() + anchor->pos() + measure->pos()
            + PointF(0.0, system->staff(accidental->vStaffIdx())->y()));
        const std::array<PointF, 4> initialCurvePoints = curvePoints(system, phrase);
        const double initialMinimumClearance = minimumClearanceFor(initialCurvePoints, initialAccidentalRect);
        std::array<PointF, 4> originalGripPositions;
        for (size_t i = 0; i < PRETTIFY_GRIPS.size(); ++i) {
            originalGripPositions[i] = phrase->ups(PRETTIFY_GRIPS[i]).pos();
        }
        EngravingItem* const originalStartElement = phraseSlur->startElement();
        EngravingItem* const originalEndElement = phraseSlur->endElement();
        const Fraction originalStartTick = phraseSlur->tick();
        const Fraction originalEndTick = phraseSlur->tick2();

        applyPrettifyCommand(score);
        anchor = toSegment(accidental->parentItem());
        measure = anchor->measure();
        system = measure->system();
        ASSERT_TRUE(system);
        phrase = nullptr;
        for (const auto& pair : score->spanner()) {
            Spanner* spanner = pair.second;
            if (!spanner || !spanner->isSlur()) {
                continue;
            }
            Slur* slur = toSlur(spanner);
            if (slur->up() && slur->staffIdx() == accidental->staffIdx()
                && slur->tick() < accidental->tick() && slur->tick2() > accidental->tick()) {
                for (SpannerSegment* segment : slur->spannerSegments()) {
                    if (segment->system() == system) {
                        phrase = toSlurSegment(segment);
                        break;
                    }
                }
            }
        }
        ASSERT_TRUE(phrase);
        const RectF accidentalRect = accidental->ldata()->bbox().translated(
            accidental->pos() + anchor->pos() + measure->pos()
            + PointF(0.0, system->staff(accidental->vStaffIdx())->y()));
        ASSERT_FALSE(accidentalRect.isNull());
        const std::array<PointF, 4> points = curvePoints(system, phrase);
        EXPECT_EQ(phraseSlur->startElement(), originalStartElement);
        EXPECT_EQ(phraseSlur->endElement(), originalEndElement);
        EXPECT_EQ(phraseSlur->tick(), originalStartTick);
        EXPECT_EQ(phraseSlur->tick2(), originalEndTick);
        const double forceResetControlMove = std::max(
            std::hypot(phrase->ups(Grip::BEZIER1).pos().x() - originalGripPositions[1].x(),
                       phrase->ups(Grip::BEZIER1).pos().y() - originalGripPositions[1].y()),
            std::hypot(phrase->ups(Grip::BEZIER2).pos().x() - originalGripPositions[2].x(),
                       phrase->ups(Grip::BEZIER2).pos().y() - originalGripPositions[2].y()));
        const Page* page = system->page();
        ASSERT_TRUE(page && page->ldata());
        const RectF pageBounds = page->ldata()->bbox();
        const RectF curveBounds = phrase->pageBoundingRect();
        const double pagePadding = 0.1 * phrase->spatium();
        EXPECT_GE(curveBounds.left(), pageBounds.left() + pagePadding);
        EXPECT_LE(curveBounds.right(), pageBounds.right() - pagePadding);
        EXPECT_GE(curveBounds.top(), pageBounds.top() + pagePadding);
        EXPECT_LE(curveBounds.bottom(), pageBounds.bottom() - pagePadding);
        const staff_idx_t staffIdx = phrase->effectiveStaffIdx();
        ASSERT_NE(staffIdx, muse::nidx);
        const Skyline& skyline = system->staff(staffIdx)->skyline();
        RectF skylineSlurBounds;
        auto includeSlurBounds = [phrase, &skylineSlurBounds](const SkylineLine& line) {
            for (const ShapeElement& element : line.elements()) {
                if (element.item() == phrase) {
                    skylineSlurBounds = skylineSlurBounds.isNull()
                                        ? RectF(element) : skylineSlurBounds.united(RectF(element));
                }
            }
        };
        includeSlurBounds(skyline.north());
        includeSlurBounds(skyline.south());
        const RectF finalSlurShapeBounds = phrase->shape().bbox().translated(phrase->pos());
        ASSERT_FALSE(skylineSlurBounds.isNull());
        EXPECT_LE(skylineSlurBounds.left(), finalSlurShapeBounds.left() + 0.001);
        EXPECT_GE(skylineSlurBounds.right(), finalSlurShapeBounds.right() - 0.001);
        EXPECT_LE(skylineSlurBounds.top(), finalSlurShapeBounds.top() + 0.001);
        EXPECT_GE(skylineSlurBounds.bottom(), finalSlurShapeBounds.bottom() - 0.001);
        const Shape finalSlurShape = phrase->shape().translated(phrase->pos());
        size_t phraseSkylineElementCount = 0;
        auto expectCurrentPhraseSkylineElements = [phrase, &finalSlurShape, &phraseSkylineElementCount](const SkylineLine& line) {
            for (const ShapeElement& skylineElement : line.elements()) {
                const EngravingItem* item = skylineElement.item();
                if (!item || (item != phrase && item->parentItem() != phrase)) {
                    continue;
                }
                ++phraseSkylineElementCount;
                const bool matchesCurrentShape = std::any_of(
                    finalSlurShape.elements().cbegin(), finalSlurShape.elements().cend(),
                    [&skylineElement](const ShapeElement& shapeElement) {
                        return std::abs(skylineElement.left() - shapeElement.left()) <= 0.001
                               && std::abs(skylineElement.top() - shapeElement.top()) <= 0.001
                               && std::abs(skylineElement.right() - shapeElement.right()) <= 0.001
                               && std::abs(skylineElement.bottom() - shapeElement.bottom()) <= 0.001;
                    });
                EXPECT_TRUE(matchesCurrentShape)
                    << "stale phrase skyline element=" << skylineElement.left() << "," << skylineElement.top()
                    << "," << skylineElement.right() << "," << skylineElement.bottom();
            }
        };
        expectCurrentPhraseSkylineElements(skyline.north());
        expectCurrentPhraseSkylineElements(skyline.south());
        EXPECT_GT(phraseSkylineElementCount, 0u);
        const double minimumClearance = minimumClearanceFor(points, accidentalRect);
        const char* glyphName = symbol == u"\u266d" ? "flat" : symbol == u"\u266e" ? "natural" : "sharp";
        RecordProperty(std::string("accidental_slur_") + glyphName,
                       std::to_string(initialMinimumClearance) + "," + std::to_string(minimumClearance)
                       + "," + std::to_string(forceResetControlMove));
        ASSERT_TRUE(std::isfinite(minimumClearance));
        EXPECT_GE(minimumClearance, 0.1 * accidental->spatium())
            << "initialClearance=" << initialMinimumClearance << " finalClearance=" << minimumClearance
            << " forceResetControlMove=" << forceResetControlMove
            << " accidentalRect=" << accidentalRect.left() << "," << accidentalRect.top()
            << "," << accidentalRect.right() << "," << accidentalRect.bottom()
            << " pageBounds=" << pageBounds.left() << "," << pageBounds.top()
            << "," << pageBounds.right() << "," << pageBounds.bottom()
            << " curveBounds=" << curveBounds.left() << "," << curveBounds.top()
            << "," << curveBounds.right() << "," << curveBounds.bottom()
            << " curvePoints=" << points[0].x() << "," << points[0].y()
            << ";" << points[1].x() << "," << points[1].y()
            << ";" << points[2].x() << "," << points[2].y()
            << ";" << points[3].x() << "," << points[3].y()
            << " phrasePos=" << phrase->pos().x() << "," << phrase->pos().y()
            << " pagePos=" << page->pos().x() << "," << page->pos().y()
            << " staffY=" << system->staff(accidental->vStaffIdx())->y()
            << " font=" << accidental->font().family().id().toStdString()
            << " spatium=" << accidental->spatium();
        const PrettifySnapshot first = capturePrettifySnapshot(score);
        EXPECT_FALSE(applyPrettifyCommand(score).changed);
        EXPECT_TRUE(snapshotsEquivalent(first, capturePrettifySnapshot(score)));
        EXPECT_EQ(originalStructure, captureStructuralAssignment(score));
        delete score;
    }

    {
        // Exercise the final correction in its own coordinate frame. A complete
        // Prettify command can also change staff spacing, so absolute endpoint
        // comparisons across that command cannot isolate this stage's contract.
        MasterScore* collidingScore = ScoreRW::readScore(u"pianomania_prettify_data/ornament-accidental-slur.mscx");
        ASSERT_TRUE(collidingScore);
        mu::engraving::pm::applyPianomaniaAutoLayout(collidingScore);
        applyPrettifyCommand(collidingScore);
        StaffText* collidingAccidental = nullptr;
        for (StaffText* text : collectStaffTexts(collidingScore)) {
            if (text->plainText() == u"\u266d") {
                collidingAccidental = text;
                break;
            }
        }
        ASSERT_TRUE(collidingAccidental);
        const Segment* collidingAnchor = toSegment(collidingAccidental->parentItem());
        const Measure* collidingMeasure = collidingAnchor->measure();
        const System* collidingSystem = collidingMeasure->system();
        SlurSegment* collidingPhrase = nullptr;
        for (const auto& pair : collidingScore->spanner()) {
            if (!pair.second || !pair.second->isSlur()) {
                continue;
            }
            Slur* slur = toSlur(pair.second);
            if (!slur->up() || slur->staffIdx() != collidingAccidental->staffIdx()
                || slur->tick() >= collidingAccidental->tick() || slur->tick2() <= collidingAccidental->tick()) {
                continue;
            }
            for (SpannerSegment* segment : slur->spannerSegments()) {
                if (segment->system() == collidingSystem) {
                    collidingPhrase = toSlurSegment(segment);
                }
            }
        }
        ASSERT_TRUE(collidingPhrase);
        std::array<PointF, 4> stagePoints;
        for (size_t i = 0; i < PRETTIFY_GRIPS.size(); ++i) {
            stagePoints[i] = collidingPhrase->ups(PRETTIFY_GRIPS[i]).pos();
        }
        for (int i = 0; i < int(Grip::GRIPS); ++i) {
            collidingPhrase->ups(Grip(i)).off = PointF();
        }
        collidingPhrase->setAutoplace(true);
        ASSERT_FALSE(collidingPhrase->isEdited());
        const double stageSpatium = collidingPhrase->spatium();
        RectF syntheticAccidental = collidingAccidental->ldata()->bbox().translated(
            collidingAccidental->pos() + collidingAnchor->pos() + collidingMeasure->pos());
        auto stageClearance = [](const std::array<PointF, 4>& points, const RectF& rect) {
            double minimum = std::numeric_limits<double>::infinity();
            for (int i = 0; i <= 4000; ++i) {
                const double t = static_cast<double>(i) / 4000.0;
                const double u = 1.0 - t;
                const PointF point = points[0] * (u * u * u) + points[1] * (3.0 * u * u * t)
                                     + points[2] * (3.0 * u * t * t) + points[3] * (t * t * t);
                if (point.x() >= rect.left() && point.x() <= rect.right()) {
                    minimum = std::min(minimum, rect.top() - point.y());
                }
            }
            return minimum;
        };
        const double originalClearance = stageClearance(stagePoints, syntheticAccidental);
        ASSERT_TRUE(std::isfinite(originalClearance));
        syntheticAccidental.translate(PointF(0.0, -originalClearance - 0.30 * stageSpatium));
        EXPECT_NEAR(stageClearance(stagePoints, syntheticAccidental), -0.30 * stageSpatium, 0.001);
        const RectF higherNotation(syntheticAccidental.left(), syntheticAccidental.top() - 0.40 * stageSpatium,
                                  syntheticAccidental.width(), 0.20 * stageSpatium);
        Shape stageShapes;
        stageShapes.add(syntheticAccidental.adjusted(0.0, -0.10 * stageSpatium, 0.0, 0.0), collidingAccidental);
        stageShapes.add(higherNotation.adjusted(0.0, -0.40 * stageSpatium, 0.0, 0.0),
                        toChord(collidingPhrase->slur()->startElement())->upNote());
        struct StageFlagsScope {
            const bool previous = MScore::pianomaniaPrettifySlursFingerings;
            StageFlagsScope() { MScore::pianomaniaPrettifySlursFingerings = true; }
            ~StageFlagsScope() { MScore::pianomaniaPrettifySlursFingerings = previous; }
        } stageFlagsScope;
        const PointF startPoint = stagePoints[0];
        muse::draw::Transform stageTransform;
        stageTransform.translate(startPoint.x(), startPoint.y());
        const muse::draw::Transform fromStageCoordinates = stageTransform.inverted();
        PointF control1 = fromStageCoordinates.map(stagePoints[1]);
        PointF control2 = fromStageCoordinates.map(stagePoints[2]);
        const PointF endPoint = fromStageCoordinates.map(stagePoints[3]);
        System* positionedSystem = const_cast<System*>(collidingSystem);
        const PointF positionedSystemPos = positionedSystem->pos();
        positionedSystem->setPos(PointF());
        EXPECT_FALSE(mu::engraving::rendering::score::SlurTieLayout::clearResidualPianomaniaAccidentalStaffText(
            collidingPhrase, stageShapes, true, stageSpatium, startPoint, endPoint,
            control1, control2, stageTransform, 0.50 * stageSpatium));
        EXPECT_EQ(stageTransform.map(control1), stagePoints[1]);
        EXPECT_EQ(stageTransform.map(control2), stagePoints[2]);
        positionedSystem->setPos(positionedSystemPos);
        control1 = fromStageCoordinates.map(stagePoints[1]);
        control2 = fromStageCoordinates.map(stagePoints[2]);
        ASSERT_TRUE(mu::engraving::rendering::score::SlurTieLayout::clearResidualPianomaniaAccidentalStaffText(
            collidingPhrase, stageShapes, true, stageSpatium, startPoint, endPoint,
            control1, control2, stageTransform, 0.50 * stageSpatium));
        const std::array<PointF, 4> stageCorrected = {
            startPoint, stageTransform.map(control1), stageTransform.map(control2), stageTransform.map(endPoint)
        };
        EXPECT_GE(stageClearance(stageCorrected, syntheticAccidental), 0.10 * stageSpatium);
        EXPECT_GE(stageClearance(stageCorrected, higherNotation), 0.40 * stageSpatium);
        EXPECT_EQ(stageCorrected.front(), stagePoints.front());
        EXPECT_EQ(stageCorrected.back(), stagePoints.back());
        EXPECT_EQ(stageCorrected[1].x(), stagePoints[1].x());
        EXPECT_EQ(stageCorrected[2].x(), stagePoints[2].x());
        const double stageLift = stagePoints[1].y() - stageCorrected[1].y();
        EXPECT_GT(stageLift, 0.0);
        EXPECT_LE(stageLift, 12.0 * stageSpatium);
        EXPECT_NEAR(stageLift, stagePoints[2].y() - stageCorrected[2].y(), 0.001);
        RecordProperty("residual_stage_clearance_and_lift_sp",
                       std::to_string(stageClearance(stageCorrected, syntheticAccidental) / stageSpatium) + ","
                       + std::to_string(stageClearance(stageCorrected, higherNotation) / stageSpatium) + ","
                       + std::to_string(stageLift / stageSpatium));
        EXPECT_FALSE(mu::engraving::rendering::score::SlurTieLayout::clearResidualPianomaniaAccidentalStaffText(
            collidingPhrase, stageShapes, true, stageSpatium, startPoint, endPoint,
            control1, control2, stageTransform, 0.50 * stageSpatium));
        EXPECT_EQ(stageTransform.map(control1), stageCorrected[1]);
        EXPECT_EQ(stageTransform.map(control2), stageCorrected[2]);

        Page* stagePage = collidingSystem->page();
        ASSERT_TRUE(stagePage);
        auto currentSystemIt = std::find(stagePage->systems().begin(), stagePage->systems().end(), collidingSystem);
        ASSERT_NE(currentSystemIt, stagePage->systems().end());
        double correctedTop = std::numeric_limits<double>::infinity();
        for (int i = 0; i <= 4000; ++i) {
            const double t = static_cast<double>(i) / 4000.0;
            const double u = 1.0 - t;
            const PointF point = stageCorrected[0] * (u * u * u) + stageCorrected[1] * (3.0 * u * u * t)
                                 + stageCorrected[2] * (3.0 * u * t * t) + stageCorrected[3] * (t * t * t);
            correctedTop = std::min(correctedTop, point.y());
        }
        ASSERT_TRUE(std::isfinite(correctedTop));
        System* corridorBlocker = Factory::createSystem(stagePage);
        const PointF pageTranslation = collidingPhrase->pagePos() - collidingPhrase->pos();
        corridorBlocker->setPos(0.0, correctedTop + pageTranslation.y());
        stagePage->systems().insert(currentSystemIt, corridorBlocker);
        control1 = fromStageCoordinates.map(stagePoints[1]);
        control2 = fromStageCoordinates.map(stagePoints[2]);
        EXPECT_FALSE(mu::engraving::rendering::score::SlurTieLayout::clearResidualPianomaniaAccidentalStaffText(
            collidingPhrase, stageShapes, true, stageSpatium, startPoint, endPoint,
            control1, control2, stageTransform, 0.50 * stageSpatium));
        EXPECT_EQ(stageTransform.map(control1), stagePoints[1]);
        EXPECT_EQ(stageTransform.map(control2), stagePoints[2]);
        stagePage->systems().erase(std::find(stagePage->systems().begin(), stagePage->systems().end(), corridorBlocker));
        delete corridorBlocker;

        Shape unreachableShapes;
        unreachableShapes.add(syntheticAccidental.translated(PointF(0.0, -20.0 * stageSpatium)), collidingAccidental);
        control1 = fromStageCoordinates.map(stagePoints[1]);
        control2 = fromStageCoordinates.map(stagePoints[2]);
        EXPECT_FALSE(mu::engraving::rendering::score::SlurTieLayout::clearResidualPianomaniaAccidentalStaffText(
            collidingPhrase, unreachableShapes, true, stageSpatium, startPoint, endPoint,
            control1, control2, stageTransform, 0.50 * stageSpatium));
        EXPECT_EQ(stageTransform.map(control1), stagePoints[1]);
        EXPECT_EQ(stageTransform.map(control2), stagePoints[2]);
        collidingPhrase->ups(Grip::BEZIER1).off = PointF(0.0, 0.10 * stageSpatium);
        EXPECT_FALSE(mu::engraving::rendering::score::SlurTieLayout::clearResidualPianomaniaAccidentalStaffText(
            collidingPhrase, stageShapes, true, stageSpatium, startPoint, endPoint,
            control1, control2, stageTransform, 0.50 * stageSpatium));
        EXPECT_EQ(stageTransform.map(control1), stagePoints[1]);
        EXPECT_EQ(stageTransform.map(control2), stagePoints[2]);
        delete collidingScore;
    }

    MasterScore* authoredScore = ScoreRW::readScore(u"pianomania_prettify_data/ornament-accidental-slur.mscx");
    ASSERT_TRUE(authoredScore);
    StaffText* authoredAccidental = nullptr;
    for (StaffText* text : collectStaffTexts(authoredScore)) {
        if (text->plainText() == u"\u266d") {
            authoredAccidental = text;
            break;
        }
    }
    ASSERT_TRUE(authoredAccidental);
    mu::engraving::pm::applyPianomaniaAutoLayout(authoredScore);
    Slur* authoredSlur = nullptr;
    SlurSegment* authoredPhrase = nullptr;
    for (const auto& pair : authoredScore->spanner()) {
        Spanner* spanner = pair.second;
        if (!spanner || !spanner->isSlur()) {
            continue;
        }
        Slur* slur = toSlur(spanner);
        if (slur->up() && slur->staffIdx() == authoredAccidental->staffIdx()
            && slur->tick() < authoredAccidental->tick() && slur->tick2() > authoredAccidental->tick()) {
            authoredSlur = slur;
            authoredPhrase = toSlurSegment(slur->frontSegment());
            break;
        }
    }
    ASSERT_TRUE(authoredSlur && authoredPhrase);
    std::array<PointF, 4> authoredOffsets;
    std::array<PropertyFlags, 4> authoredOffsetFlags;
    bool hasAuthoredGrip = false;
    for (size_t i = 0; i < PRETTIFY_GRIPS.size(); ++i) {
        authoredOffsets[i] = authoredPhrase->ups(PRETTIFY_GRIPS[i]).off;
        authoredOffsetFlags[i] = authoredPhrase->propertyFlags(PRETTIFY_SLUR_PROPERTIES[i]);
        hasAuthoredGrip = hasAuthoredGrip || !authoredOffsets[i].isNull();
    }
    ASSERT_TRUE(hasAuthoredGrip);
    const bool authoredAutoplace = authoredPhrase->autoplace();
    const PropertyFlags authoredAutoplaceFlags = authoredPhrase->propertyFlags(Pid::AUTOPLACE);
    const EngravingItem* authoredStartElement = authoredSlur->startElement();
    const EngravingItem* authoredEndElement = authoredSlur->endElement();
    mu::engraving::pm::PmPrettifyOptions preserveAuthoredOptions;
    preserveAuthoredOptions.forceNormalizeManual = false;
    applyPrettifyCommand(authoredScore, preserveAuthoredOptions);
    authoredPhrase = toSlurSegment(authoredSlur->frontSegment());
    ASSERT_TRUE(authoredPhrase);
    EXPECT_EQ(authoredSlur->startElement(), authoredStartElement);
    EXPECT_EQ(authoredSlur->endElement(), authoredEndElement);
    EXPECT_EQ(authoredPhrase->autoplace(), authoredAutoplace);
    EXPECT_EQ(authoredPhrase->propertyFlags(Pid::AUTOPLACE), authoredAutoplaceFlags);
    for (size_t i = 0; i < PRETTIFY_GRIPS.size(); ++i) {
        EXPECT_EQ(authoredPhrase->ups(PRETTIFY_GRIPS[i]).off, authoredOffsets[i]);
        EXPECT_EQ(authoredPhrase->propertyFlags(PRETTIFY_SLUR_PROPERTIES[i]), authoredOffsetFlags[i]);
    }
    delete authoredScore;

    MasterScore* manualScore = ScoreRW::readScore(u"pianomania_prettify_data/ornament-accidental-slur.mscx");
    ASSERT_TRUE(manualScore);
    StaffText* manualAccidental = nullptr;
    for (StaffText* text : collectStaffTexts(manualScore)) {
        if (text->plainText() == u"\u266d") {
            manualAccidental = text;
            break;
        }
    }
    ASSERT_TRUE(manualAccidental);
    mu::engraving::pm::applyPianomaniaAutoLayout(manualScore);
    SlurSegment* manualPhrase = nullptr;
    for (const auto& pair : manualScore->spanner()) {
        Spanner* spanner = pair.second;
        if (!spanner || !spanner->isSlur()) {
            continue;
        }
        Slur* slur = toSlur(spanner);
        if (slur->up() && slur->staffIdx() == manualAccidental->staffIdx()
            && slur->tick() < manualAccidental->tick() && slur->tick2() > manualAccidental->tick()) {
            manualPhrase = toSlurSegment(slur->frontSegment());
            break;
        }
    }
    ASSERT_TRUE(manualPhrase);
    manualPhrase->setAutoplace(false);
    Slur* manualSlur = manualPhrase->slur();
    std::array<PointF, 4> manualOffsets;
    std::array<PropertyFlags, 4> manualOffsetFlags;
    for (size_t i = 0; i < PRETTIFY_GRIPS.size(); ++i) {
        manualOffsets[i] = manualPhrase->ups(PRETTIFY_GRIPS[i]).off;
        manualOffsetFlags[i] = manualPhrase->propertyFlags(PRETTIFY_SLUR_PROPERTIES[i]);
    }
    mu::engraving::pm::PmPrettifyOptions preserveManualOptions;
    preserveManualOptions.forceNormalizeManual = false;
    applyPrettifyCommand(manualScore, preserveManualOptions);
    manualPhrase = toSlurSegment(manualSlur->frontSegment());
    ASSERT_TRUE(manualPhrase);
    EXPECT_FALSE(manualPhrase->autoplace());
    for (size_t i = 0; i < PRETTIFY_GRIPS.size(); ++i) {
        EXPECT_EQ(manualPhrase->ups(PRETTIFY_GRIPS[i]).off, manualOffsets[i]);
        EXPECT_EQ(manualPhrase->propertyFlags(PRETTIFY_SLUR_PROPERTIES[i]), manualOffsetFlags[i]);
    }
    delete manualScore;
}

// Test value: Keeps expression words, mixed text and hidden accidental glyphs
// out of the notation obstacle path without relying on the glyph classifier.
TEST_F(Engraving_PianomaniaPrettifyTests, ignoredStaffTextDoesNotShapePhraseSlur)
{
    for (int scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario);
        std::array<std::vector<PointF>, 2> curves;
        for (int variant = 0; variant < 2; ++variant) {
            MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/ornament-accidental-slur.mscx");
            ASSERT_TRUE(score);
            StaffText* accidental = nullptr;
            for (StaffText* text : collectStaffTexts(score)) {
                if (text->plainText() == u"\u266d") {
                    accidental = text;
                    break;
                }
            }
            ASSERT_TRUE(accidental);
            if (variant == 0) {
                if (scenario == 0) {
                    accidental->setXmlText(u"dolce");
                } else if (scenario == 1) {
                    accidental->setXmlText(u"dolce \u266d");
                } else {
                    accidental->setVisible(false);
                }
            } else {
                score->removeElement(accidental);
                delete accidental;
            }
            mu::engraving::pm::applyPianomaniaAutoLayout(score);
            applyPrettifyCommand(score);
            for (const auto& pair : score->spanner()) {
                if (!pair.second || !pair.second->isSlur()) {
                    continue;
                }
                for (SpannerSegment* segment : toSlur(pair.second)->spannerSegments()) {
                    for (Grip grip : PRETTIFY_GRIPS) {
                        curves[variant].push_back(toSlurSegment(segment)->ups(grip).pos());
                    }
                }
            }
            delete score;
        }
        ASSERT_EQ(curves[0].size(), curves[1].size());
        ASSERT_FALSE(curves[0].empty());
        for (size_t i = 0; i < curves[0].size(); ++i) {
            EXPECT_TRUE(pointNear(curves[0][i], curves[1][i], 0.01));
        }
    }
}

TEST_F(Engraving_PianomaniaPrettifyTests, prettifyIsIdempotentAndUndoable)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/prettify-idempotency.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    const PrettifySnapshot originalSnapshot = capturePrettifySnapshot(score);
    const StructuralAssignment originalStructure = captureStructuralAssignment(score);

    const mu::engraving::pm::PmPrettifyResult firstResult = applyPrettifyCommand(score);
    EXPECT_TRUE(firstResult.changed);
    EXPECT_FALSE(firstResult.structuralAssignmentChanged);
    EXPECT_GE(firstResult.normalizedManualSlurs, 1);
    EXPECT_GE(firstResult.normalizedManualFingerings, 1);

    const PrettifySnapshot firstSnapshot = capturePrettifySnapshot(score);
    const StructuralAssignment firstStructure = captureStructuralAssignment(score);
    EXPECT_FALSE(slurSnapshotsEquivalent(originalSnapshot.slurSegments, firstSnapshot.slurSegments));
    EXPECT_FALSE(fingeringSnapshotsEquivalent(originalSnapshot.fingerings, firstSnapshot.fingerings));
    EXPECT_EQ(originalStructure, firstStructure);

    const mu::engraving::pm::PmPrettifyResult secondResult = applyPrettifyCommand(score);
    EXPECT_FALSE(secondResult.changed);
    EXPECT_FALSE(secondResult.structuralAssignmentChanged);
    EXPECT_TRUE(snapshotsEquivalent(firstSnapshot, capturePrettifySnapshot(score)));
    EXPECT_EQ(firstStructure, captureStructuralAssignment(score));

    for (int cycle = 0; cycle < 5; ++cycle) {
        SCOPED_TRACE(cycle);

        EditData undoEditData;
        score->undoStack()->undo(&undoEditData);
        relayoutScore(score);

        EXPECT_TRUE(snapshotsEquivalent(originalSnapshot, capturePrettifySnapshot(score)));
        EXPECT_EQ(originalStructure, captureStructuralAssignment(score));

        EditData redoEditData;
        score->undoStack()->redo(&redoEditData);
        relayoutScore(score);

        EXPECT_TRUE(snapshotsEquivalent(firstSnapshot, capturePrettifySnapshot(score)));
        EXPECT_EQ(firstStructure, captureStructuralAssignment(score));
    }

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, fingeringClearsVisibleRestObstacle)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-rest-obstacle.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    const mu::engraving::pm::PmPrettifyResult result = applyPrettifyCommand(score);
    EXPECT_TRUE(result.changed);

    const std::vector<Fingering*> fingerings = collectFingeringsByText(score, u"1");
    ASSERT_EQ(fingerings.size(), 1);
    const std::vector<Rest*> rests = collectVisibleRests(score);
    ASSERT_FALSE(rests.empty());

    const RectF fingeringRect = fingeringSystemRect(fingerings.front());
    for (const Rest* rest : rests) {
        EXPECT_FALSE(rectsOverlap(fingeringRect, restSystemRect(rest)));
    }

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, sparseCrossVoiceFingeringKeepsNoteOwnership)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-cross-voice-ownership.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);
    const StructuralAssignment originalStructure = captureStructuralAssignment(score);
    const PrettifySnapshot original = capturePrettifySnapshot(score);

    const auto result = applyPrettifyCommand(score);
    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(result.structuralAssignmentChanged);
    const auto fingerings = collectFingeringsByText(score, u"4");
    const auto ownerDigit = std::find_if(fingerings.begin(), fingerings.end(), [](const Fingering* fingering) {
        return fingering->note()->pitch() == 55;
    });
    ASSERT_NE(ownerDigit, fingerings.end());
    const Fingering* fingering = *ownerDigit;
    const Note* owner = fingering->note();
    const Segment* segment = owner->chord()->segment();
    const Note* other = nullptr;
    for (const EngravingItem* item : segment->elist()) {
        if (item && item->isChord() && item->vStaffIdx() == owner->chord()->vStaffIdx()) {
            for (const Note* note : toChord(item)->notes()) {
                if (note->pitch() == 59) {
                    other = note;
                }
            }
        }
    }
    ASSERT_TRUE(other);
    auto assertOwnership = [&]() {
        EXPECT_EQ(fingering->note(), owner);
        const RectF digitRect = fingeringSystemRect(fingering);
        const RectF ownerRect = noteSystemRect(owner);
        const RectF otherRect = noteSystemRect(other);
        const double clearance = 0.15 * fingering->spatium();
        EXPECT_FALSE(rectsOverlap(digitRect.adjusted(-clearance, -clearance, clearance, clearance), otherRect));
        EXPECT_FALSE(rectsOverlap(digitRect.adjusted(-clearance, -clearance, clearance, clearance), ownerRect));
        // An isolated digit above B would visually assign it to B. It must
        // remain closer in pitch height to its G, alongside or below G.
        EXPECT_LT(std::abs(digitRect.center().y() - ownerRect.center().y()),
                  std::abs(digitRect.center().y() - otherRect.center().y()));
        EXPECT_EQ(originalStructure, captureStructuralAssignment(score));
    };
    assertOwnership();
    const PrettifySnapshot prettified = capturePrettifySnapshot(score);
    const auto repeated = applyPrettifyCommand(score);
    EXPECT_FALSE(repeated.changed);
    assertOwnership();
    EditData editData;
    score->undoStack()->undo(&editData);
    relayoutScore(score);
    EXPECT_TRUE(snapshotsEquivalent(original, capturePrettifySnapshot(score)));
    score->undoStack()->redo(&editData);
    relayoutScore(score);
    EXPECT_TRUE(snapshotsEquivalent(prettified, capturePrettifySnapshot(score)));
    assertOwnership();
    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, sparseChordFingeringsKeepOwnershipOnEitherSide)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-chord-ownership.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);
    const StructuralAssignment structure = captureStructuralAssignment(score);
    const auto result = applyPrettifyCommand(score);
    EXPECT_FALSE(result.structuralAssignmentChanged);
    for (const String& text : { String(u"4"), String(u"1") }) {
        const auto fingerings = collectFingeringsByText(score, text);
        ASSERT_EQ(fingerings.size(), 1);
        const Fingering* fingering = fingerings.front();
        const Note* owner = fingering->note();
        const RectF digitRect = fingeringSystemRect(fingering);
        const RectF ownerRect = noteSystemRect(owner);
        const double clearance = 0.15 * fingering->spatium();
        for (const Note* note : owner->chord()->notes()) {
            const RectF noteRect = noteSystemRect(note);
            EXPECT_FALSE(rectsOverlap(digitRect.adjusted(-clearance, -clearance, clearance, clearance), noteRect));
            if (note != owner) {
                EXPECT_LT(std::abs(digitRect.center().y() - ownerRect.center().y()),
                          std::abs(digitRect.center().y() - noteRect.center().y()));
            }
        }
    }
    EXPECT_EQ(structure, captureStructuralAssignment(score));
    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, fingeringClearsRenderedStemsBeamsAndFlags)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-structural-clearance.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    const PrettifySnapshot original = capturePrettifySnapshot(score);
    ASSERT_FALSE(original.fingerings.empty());
    std::vector<const Note*> owners;
    owners.reserve(original.fingerings.size());
    for (const FingeringSnapshotEntry& entry : original.fingerings) {
        owners.push_back(entry.fingering->note());
    }

    const mu::engraving::pm::PmPrettifyResult result = applyPrettifyCommand(score);
    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(result.structuralAssignmentChanged);

    bool sawBeam = false;
    bool sawFlag = false;
    bool sawChordOwner = false;
    bool sawSecondVoice = false;
    bool sawDigitLeftOfStem = false;
    bool sawDigitRightOfStem = false;
    for (size_t i = 0; i < original.fingerings.size(); ++i) {
        const Fingering* fingering = original.fingerings[i].fingering;
        ASSERT_EQ(fingering->note(), owners[i]);
        const Note* owner = fingering->note();
        const Chord* chord = owner ? owner->chord() : nullptr;
        ASSERT_TRUE(chord);
        sawChordOwner = sawChordOwner || chord->notes().size() > 1;
        sawSecondVoice = sawSecondVoice || chord->voice() > 0;
        sawBeam = sawBeam || chord->beam();
        sawFlag = sawFlag || chord->hook();

        const RectF digitRect = fingeringSystemRect(fingering);
        const double clearance = 0.25 * fingering->spatium() - 1e-4;
        const RectF paddedDigit = digitRect.adjusted(-clearance, -clearance, clearance, clearance);
        for (const RectF& structuralRect : fingeringOwnerStructuralRects(fingering)) {
            EXPECT_FALSE(rectsOverlap(paddedDigit, structuralRect)) << fingering->plainText().toStdString();
        }

        const Stem* stem = chord->stem();
        if (stem && stem->visible() && stem->ldata() && !stem->ldata()->isSkipDraw()) {
            const Segment* segment = chord->segment();
            const Measure* measure = segment ? segment->measure() : nullptr;
            ASSERT_TRUE(measure);
            const RectF stemRect = stem->ldata()->bbox().translated(
                PointF(0.0, staffYInSystem(measure->system(), chord->vStaffIdx()))
                + stem->pos() + chord->pos() + segment->pos() + measure->pos() + stem->staffOffset());
            sawDigitLeftOfStem = sawDigitLeftOfStem || digitRect.center().x() < stemRect.center().x();
            sawDigitRightOfStem = sawDigitRightOfStem || digitRect.center().x() > stemRect.center().x();
        }
    }

    EXPECT_TRUE(sawBeam);
    EXPECT_TRUE(sawFlag);
    EXPECT_TRUE(sawChordOwner);
    EXPECT_TRUE(sawSecondVoice);
    EXPECT_TRUE(sawDigitLeftOfStem);
    EXPECT_TRUE(sawDigitRightOfStem);

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, lowerStaffFingeringsClearSameStaffBeamInSystemFrame)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-lower-staff-beam-clearance.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    const StructuralAssignment structure = captureStructuralAssignment(score);
    const PrettifySnapshot original = capturePrettifySnapshot(score);
    ASSERT_EQ(original.fingerings.size(), 4);
    std::vector<const Note*> owners;
    for (const FingeringSnapshotEntry& entry : original.fingerings) {
        owners.push_back(entry.fingering->note());
    }

    auto assertAcceptedPlacement = [&]() {
        EXPECT_EQ(structure, captureStructuralAssignment(score));
        for (size_t i = 0; i < original.fingerings.size(); ++i) {
            const Fingering* fingering = original.fingerings[i].fingering;
            ASSERT_EQ(fingering->note(), owners[i]);
            ASSERT_TRUE(fingering->note());
            const Chord* chord = fingering->note()->chord();
            ASSERT_TRUE(chord);
            ASSERT_EQ(chord->vStaffIdx(), staff_idx_t(1));
            const Beam* beam = chord->beam();
            ASSERT_TRUE(beam);
            ASSERT_TRUE(beam->visible());
            ASSERT_FALSE(beam->cross());
            ASSERT_FALSE(beam->fullCross());
            ASSERT_FALSE(beam->beamSegments().empty());
            for (const ChordRest* element : beam->elements()) {
                ASSERT_TRUE(element);
                EXPECT_EQ(element->staffMove(), 0);
            }
            const Measure* measure = chord->measure();
            ASSERT_TRUE(measure);
            const System* system = measure->system();
            ASSERT_TRUE(system);
            const PointF renderedBeamOrigin = beam->pagePos() - system->pagePos();
            EXPECT_GT(renderedBeamOrigin.y(), 0.0);
            EXPECT_NEAR(renderedBeamOrigin.y(), staffYInSystem(system, beam->staffIdx()), 1e-4);

            const RectF digitRect = fingeringSystemRect(fingering);
            const RectF ownerRect = noteSystemRect(fingering->note());
            EXPECT_LT(digitRect.center().y(), ownerRect.center().y());
            const double clearance = 0.25 * fingering->spatium() - 1e-4;
            const RectF paddedDigit = digitRect.adjusted(-clearance, -clearance, clearance, clearance);
            const std::vector<RectF> structuralRects = fingeringOwnerStructuralRects(fingering);
            ASSERT_FALSE(structuralRects.empty());
            for (const RectF& structuralRect : structuralRects) {
                EXPECT_FALSE(rectsOverlap(paddedDigit, structuralRect)) << fingering->plainText().toStdString();
            }
        }
    };

    const auto result = applyPrettifyCommand(score);
    EXPECT_FALSE(result.structuralAssignmentChanged);
    assertAcceptedPlacement();
    const auto repeated = applyPrettifyCommand(score);
    EXPECT_FALSE(repeated.changed);
    EXPECT_FALSE(repeated.structuralAssignmentChanged);
    assertAcceptedPlacement();

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, tupletBlockedFingeringFlipsToClearNoteheadSide)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-tuplet-obstacle.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    const mu::engraving::pm::PmPrettifyResult result = applyPrettifyCommand(score);
    EXPECT_TRUE(result.changed);

    std::vector<Fingering*> fingerings;
    for (const String& text : { String(u"1"), String(u"2"), String(u"3") }) {
        std::vector<Fingering*> matching = collectFingeringsByText(score, text);
        ASSERT_EQ(matching.size(), 1);
        fingerings.push_back(matching.front());
    }
    const std::vector<Tuplet*> tuplets = collectTuplets(score);
    ASSERT_EQ(tuplets.size(), 1);

    RectF fingeringRect;
    for (Fingering* fingering : fingerings) {
        EXPECT_EQ(fingering->placement(), PlacementV::ABOVE);
        fingeringRect.unite(fingering->pageBoundingRect());
    }

    const RectF tupletRect = tupletNumberSystemRect(tuplets.front());
    ASSERT_FALSE(fingeringRect.isNull());
    ASSERT_FALSE(tupletRect.isNull());
    EXPECT_LT(fingeringRect.bottom(), tupletRect.top());
    EXPECT_FALSE(rectsOverlap(fingeringRect, tupletRect));

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, fingeringPlacementStaysWithinNoteheadCap)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-notehead-cap.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    const mu::engraving::pm::PmPrettifyResult result = applyPrettifyCommand(score);
    EXPECT_TRUE(result.changed);

    constexpr double capSp = 3.0;
    for (const String& text : { String(u"1"), String(u"2"), String(u"3"), String(u"4") }) {
        const std::vector<Fingering*> matching = collectFingeringsByText(score, text);
        ASSERT_EQ(matching.size(), 1);
        const Fingering* fingering = matching.front();
        EXPECT_LE(fingeringNoteheadDistance(fingering), capSp * std::max(1.0, fingering->spatium()) + 0.05)
            << text.toStdString();
    }

    delete score;
}

// Test value: Moonlight m29/m46/m57. A beamed lower-voice digit stranded
// across the staff body (above the upper voice's rests) or past its own beam
// is seated in the stem-side pocket instead: just under its notehead, beside
// the down-stem, clear of the beam and off the staff lines. Every digit of a
// beam moves together, and the seat survives a second Prettify.
TEST_F(Engraving_PianomaniaPrettifyTests, strandedBeamedDigitsNestleInStemSidePocket)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-stem-side-nestle.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    const StructuralAssignment structure = captureStructuralAssignment(score);
    const PrettifySnapshot original = capturePrettifySnapshot(score);
    ASSERT_EQ(original.fingerings.size(), 6);
    std::vector<const Note*> owners;
    for (const FingeringSnapshotEntry& entry : original.fingerings) {
        owners.push_back(entry.fingering->note());
    }

    auto assertNestled = [&]() {
        EXPECT_EQ(structure, captureStructuralAssignment(score));
        for (size_t i = 0; i < original.fingerings.size(); ++i) {
            const Fingering* fingering = original.fingerings[i].fingering;
            const std::string label = fingering->plainText().toStdString();
            ASSERT_EQ(fingering->note(), owners[i]) << label;
            const Chord* chord = fingering->note()->chord();
            const Beam* beam = chord->beam();
            const Stem* stem = chord->stem();
            ASSERT_TRUE(beam && stem) << label;
            ASSERT_FALSE(chord->up()) << label;
            const Measure* measure = chord->measure();
            const System* system = measure->system();
            const double sp = fingering->spatium();
            const double staffTop = staffYInSystem(system, chord->vStaffIdx());
            const double staffBottom = staffTop + chord->staff()->staffHeight(chord->tick());

            EXPECT_EQ(fingering->placement(), PlacementV::BELOW) << label;
            const RectF digitRect = fingeringSystemRect(fingering);
            const RectF noteRect = noteSystemRect(fingering->note());
            const RectF stemRect = stem->ldata()->bbox().translated(
                PointF(0.0, staffTop) + stem->pos() + chord->pos() + chord->segment()->pos() + measure->pos());

            // Just under its own notehead, off the staff lines.
            EXPECT_GE(digitRect.top(), noteRect.bottom() + 0.15 * sp - 1e-3) << label;
            EXPECT_LE(digitRect.top() - noteRect.bottom(), 1.5 * sp + 1e-3) << label;
            EXPECT_GE(digitRect.top(), staffBottom + 0.1 * sp - 1e-3) << label;
            // Beside the down-stem rather than under it, still under the notehead.
            EXPECT_GE(digitRect.left(), stemRect.right() + 0.25 * sp - 1e-3) << label;
            EXPECT_LT(digitRect.left(), noteRect.right()) << label;
            // Above the drawn beam band with structural clearance.
            const PointF beamOrigin = beam->pagePos() - system->pagePos();
            for (const BeamSegment* beamSegment : beam->beamSegments()) {
                const PointF start = beamSegment->line.p1() + beamOrigin;
                const PointF end = beamSegment->line.p2() + beamOrigin;
                for (double x : { digitRect.left(), digitRect.center().x(), digitRect.right() }) {
                    if (x < std::min(start.x(), end.x()) || x > std::max(start.x(), end.x())) {
                        continue;
                    }
                    const double centerY = start.y() + (end.y() - start.y()) * (x - start.x()) / (end.x() - start.x());
                    EXPECT_GE(centerY - 0.5 * beam->beamWidth(), digitRect.bottom() + 0.25 * sp - 1e-3) << label;
                }
            }
        }
    };

    const auto result = applyPrettifyCommand(score);
    EXPECT_TRUE(result.changed);
    EXPECT_FALSE(result.structuralAssignmentChanged);
    assertNestled();
    const auto repeated = applyPrettifyCommand(score);
    EXPECT_FALSE(repeated.changed);
    assertNestled();

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, detachedFingeringRescueClearsRealStaffBoundaryAndPersists)
{
    const String fixture = u"pianomania_prettify_data/detached-fingering-real-staff-boundary.mscx";
    MasterScore* score = ScoreRW::readScore(fixture);
    ASSERT_TRUE(score);
    relayoutScore(score);

    auto targetFingerings = [](Score* current) {
        std::array<Fingering*, 2> result { nullptr, nullptr };
        const std::vector<Fingering*> oneToFive = collectFingeringsByText(current, u"1-5");
        if (oneToFive.size() == 1) {
            result[1] = oneToFive.front();
        }
        for (Fingering* fingering : collectFingeringsByText(current, u"2")) {
            const Note* note = fingering ? fingering->note() : nullptr;
            const Chord* chord = note ? note->chord() : nullptr;
            if (note && chord && note->pitch() == 60 && chord->voice() == 0 && fingering->tick() > Fraction(0, 1)) {
                result[0] = fingering;
                break;
            }
        }
        return result;
    };
    auto expectRealStaffClearance = [](const std::array<Fingering*, 2>& fingerings) {
        for (const Fingering* fingering : fingerings) {
            ASSERT_TRUE(fingering);
            ASSERT_TRUE(fingering->note());
            ASSERT_TRUE(fingering->note()->chord());
            const Chord* chord = fingering->note()->chord();
            ASSERT_TRUE(chord->measure());
            ASSERT_TRUE(chord->measure()->system());
            EXPECT_EQ(fingering->placement(), PlacementV::ABOVE);
            const double staffTop = chord->measure()->pos().y()
                                    + staffYInSystem(chord->measure()->system(), chord->vStaffIdx());
            const double minimumClearance = 0.10 * fingering->spatium();
            EXPECT_LE(fingeringSystemRect(fingering).bottom(), staffTop - minimumClearance + 1e-4)
                << fingering->plainText().toStdString();
        }
    };

    const auto first = applyPrettifyCommand(score);
    EXPECT_TRUE(first.changed);
    std::array<Fingering*, 2> fingerings = targetFingerings(score);
    ASSERT_TRUE(fingerings[0]);
    ASSERT_TRUE(fingerings[1]);
    expectRealStaffClearance(fingerings);

    SlurSegment* nearbySlur = firstSlurSegment(score);
    ASSERT_TRUE(nearbySlur);
    const RectF nearbySlurRect = nearbySlur->shape().bbox().translated(
        nearbySlur->pos() + PointF(0.0, staffYInSystem(nearbySlur->system(), nearbySlur->vStaffIdx())));
    EXPECT_LT(fingeringSystemRect(fingerings[1]).right(), nearbySlurRect.left());

    const std::array<PointF, 2> acceptedPositions { fingerings[0]->pagePos(), fingerings[1]->pagePos() };
    const auto repeated = applyPrettifyCommand(score);
    EXPECT_FALSE(repeated.changed);
    fingerings = targetFingerings(score);
    expectRealStaffClearance(fingerings);
    for (size_t i = 0; i < fingerings.size(); ++i) {
        EXPECT_TRUE(pointNear(fingerings[i]->pagePos(), acceptedPositions[i], 0.02 * fingerings[i]->spatium()));
    }

    const std::string savedFileName = testing::TempDir() + "detached-fingering-real-staff-boundary-roundtrip.mscx";
    const String savedPath = String::fromUtf8(savedFileName);
    ASSERT_TRUE(ScoreRW::saveScore(score, savedPath));
    MasterScore* reloaded = ScoreRW::readScore(savedPath, true);
    ASSERT_TRUE(reloaded);
    relayoutScore(reloaded);
    fingerings = targetFingerings(reloaded);
    expectRealStaffClearance(fingerings);
    for (size_t i = 0; i < fingerings.size(); ++i) {
        EXPECT_TRUE(pointNear(fingerings[i]->pagePos(), acceptedPositions[i], 0.02 * fingerings[i]->spatium()));
    }
    const auto reloadedRepeated = applyPrettifyCommand(reloaded);
    EXPECT_FALSE(reloadedRepeated.changed);
    expectRealStaffClearance(targetFingerings(reloaded));

    delete reloaded;
    std::remove(savedFileName.c_str());
    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, prettifyButtonPersistsFingeringsAndTempoClearance)
{
    struct PrettifyFlagsOff {
        bool previousPrettify = false;
        bool previousForceNormalize = false;

        PrettifyFlagsOff()
        {
            previousPrettify = MScore::pianomaniaPrettifySlursFingerings;
            previousForceNormalize = MScore::pianomaniaForceNormalizeSlursFingerings;
            MScore::pianomaniaPrettifySlursFingerings = false;
            MScore::pianomaniaForceNormalizeSlursFingerings = false;
        }

        ~PrettifyFlagsOff()
        {
            MScore::pianomaniaPrettifySlursFingerings = previousPrettify;
            MScore::pianomaniaForceNormalizeSlursFingerings = previousForceNormalize;
        }
    } prettifyFlagsOff;

    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-tempo-slur-pocket.mscx");
    ASSERT_TRUE(score);
    const std::vector<TempoSnapshotEntry> originalTempo = captureTempoSnapshot(score);
    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    const std::vector<TempoText*> tempos = collectTempoTexts(score);
    ASSERT_EQ(tempos.size(), 1);
    TempoText* tempo = tempos.front();
    const PointF originalTempoOffset = tempo->offset();
    const PropertyFlags originalTempoOffsetFlags = tempo->propertyFlags(Pid::OFFSET);
    const OffsetChange originalTempoOffsetChanged = tempo->ldata()->offsetChanged();
    const PointF originalTempoChangedPos = tempo->ldata()->autoplace.changedPos;
    const mu::engraving::pm::PmPrettifyResult result = applyPrettifyCommand(score);
    EXPECT_TRUE(result.changed);

    std::vector<Fingering*> rightHand;
    for (const FingeringSnapshotEntry& entry : capturePrettifySnapshot(score).fingerings) {
        const Note* note = entry.fingering ? entry.fingering->note() : nullptr;
        const Chord* chord = note ? note->chord() : nullptr;
        if (chord && chord->vStaffIdx() == 0) {
            rightHand.push_back(entry.fingering);
        }
    }
    std::sort(rightHand.begin(), rightHand.end(), [](const Fingering* left, const Fingering* right) {
        return left->tick() < right->tick();
    });
    ASSERT_GE(rightHand.size(), 6);

    constexpr std::array<int, 6> expectedPitches = { 62, 67, 69, 70, 72, 74 };
    // The slur and staff edge make the strict 3sp preference unreachable for
    // some of these notes. The repair must still bring the opening run into a
    // bounded, staff-adjacent pocket instead of leaving it near the page top.
    // The run is beamed, and the fingerings sit above a beam that rises with
    // the melody, so the middle notes' fingerings land about 4.75sp away.
    constexpr double maximumSafeDetachmentSp = 5.0;
    for (size_t i = 0; i < expectedPitches.size(); ++i) {
        ASSERT_TRUE(rightHand[i]->note());
        EXPECT_EQ(rightHand[i]->note()->pitch(), expectedPitches[i]);
        EXPECT_EQ(rightHand[i]->placement(), PlacementV::ABOVE);
        if (i < 4) {
            EXPECT_LE(fingeringNoteheadDistance(rightHand[i]),
                      maximumSafeDetachmentSp * std::max(1.0, rightHand[i]->spatium()) + 0.05) << i;
        }
    }

    ASSERT_TRUE(tempo->visible());
    const RectF tempoRect = tempo->pageBoundingRect();
    const double clearance = 0.25 * std::max(1.0, rightHand.front()->spatium());
    for (size_t i = 0; i < 4; ++i) {
        const RectF fingeringRect = rightHand[i]->pageBoundingRect();
        if (fingeringRect.right() > tempoRect.left() && fingeringRect.left() < tempoRect.right()
            && fingeringRect.center().y() > tempoRect.center().y()) {
            EXPECT_GE(fingeringRect.top() - tempoRect.bottom(), clearance - 0.05) << i;
        }
    }
    EXPECT_TRUE(tempoSnapshotsEquivalent(captureTempoSnapshot(score), originalTempo));

    const PointF acceptedOne = rightHand[4]->pagePos();
    const PointF acceptedTwo = rightHand[5]->pagePos();
    const PointF repairedTempoOffset = tempo->offset();
    const PropertyFlags repairedTempoOffsetFlags = tempo->propertyFlags(Pid::OFFSET);
    const OffsetChange repairedTempoOffsetChanged = tempo->ldata()->offsetChanged();
    const PointF repairedTempoChangedPos = tempo->ldata()->autoplace.changedPos;
    const PointF tempoPosition = tempo->pagePos();
    relayoutScore(score);
    EXPECT_TRUE(pointNear(tempo->pagePos(), tempoPosition, 0.02 * tempo->spatium()));
    EXPECT_TRUE(pointNear(rightHand[4]->pagePos(), acceptedOne, 0.02 * rightHand[4]->spatium()));
    EXPECT_TRUE(pointNear(rightHand[5]->pagePos(), acceptedTwo, 0.02 * rightHand[5]->spatium()));
    const RectF retainedTempoRect = tempo->pageBoundingRect();
    for (size_t i = 0; i < 4; ++i) {
        const RectF fingeringRect = rightHand[i]->pageBoundingRect();
        if (fingeringRect.right() > retainedTempoRect.left() && fingeringRect.left() < retainedTempoRect.right()
            && fingeringRect.center().y() > retainedTempoRect.center().y()) {
            EXPECT_GE(fingeringRect.top() - retainedTempoRect.bottom(), clearance - 0.05) << i;
        }
    }
    EditData undoEditData;
    score->undoStack()->undo(&undoEditData);
    relayoutScore(score);
    EXPECT_EQ(tempo->offset(), originalTempoOffset);
    EXPECT_EQ(tempo->propertyFlags(Pid::OFFSET), originalTempoOffsetFlags);
    EXPECT_EQ(tempo->ldata()->offsetChanged(), originalTempoOffsetChanged);
    EXPECT_EQ(tempo->ldata()->autoplace.changedPos, originalTempoChangedPos);
    EXPECT_TRUE(tempoSnapshotsEquivalent(captureTempoSnapshot(score), originalTempo));

    EditData redoEditData;
    score->undoStack()->redo(&redoEditData);
    relayoutScore(score);
    EXPECT_EQ(tempo->offset(), repairedTempoOffset);
    EXPECT_EQ(tempo->propertyFlags(Pid::OFFSET), repairedTempoOffsetFlags);
    EXPECT_EQ(tempo->ldata()->offsetChanged(), repairedTempoOffsetChanged);
    EXPECT_EQ(tempo->ldata()->autoplace.changedPos, repairedTempoChangedPos);
    EXPECT_TRUE(pointNear(tempo->pagePos(), tempoPosition, 0.02 * tempo->spatium()));
    EXPECT_TRUE(pointNear(rightHand[4]->pagePos(), acceptedOne, 0.02 * rightHand[4]->spatium()));
    EXPECT_TRUE(pointNear(rightHand[5]->pagePos(), acceptedTwo, 0.02 * rightHand[5]->spatium()));

    std::array<PointF, 4> openingPositions;
    for (size_t i = 0; i < openingPositions.size(); ++i) {
        openingPositions[i] = rightHand[i]->pagePos();
    }
    applyPrettifyCommand(score);
    EXPECT_EQ(tempo->offset(), repairedTempoOffset);
    EXPECT_EQ(tempo->propertyFlags(Pid::OFFSET), repairedTempoOffsetFlags);
    EXPECT_EQ(tempo->ldata()->offsetChanged(), repairedTempoOffsetChanged);
    EXPECT_EQ(tempo->ldata()->autoplace.changedPos, repairedTempoChangedPos);
    EXPECT_TRUE(pointNear(tempo->pagePos(), tempoPosition, 0.02 * tempo->spatium()));
    for (size_t i = 0; i < openingPositions.size(); ++i) {
        EXPECT_TRUE(pointNear(rightHand[i]->pagePos(), openingPositions[i], 0.02 * rightHand[i]->spatium())) << i;
    }
    EXPECT_TRUE(pointNear(rightHand[4]->pagePos(), acceptedOne, 0.02 * rightHand[4]->spatium()));
    EXPECT_TRUE(pointNear(rightHand[5]->pagePos(), acceptedTwo, 0.02 * rightHand[5]->spatium()));
    EXPECT_TRUE(tempoSnapshotsEquivalent(captureTempoSnapshot(score), originalTempo));

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, textHairpinClearsNotationAndFingerings)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/text-hairpin-notation-collision.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    std::vector<HairpinSegment*> hairpinSegments = collectLineHairpinSegments(score);
    ASSERT_EQ(hairpinSegments.size(), 1);
    HairpinSegment* hairpinSegment = hairpinSegments.front();
    ASSERT_EQ(hairpinSegment->hairpin()->hairpinType(), HairpinType::CRESC_LINE);

    const staff_idx_t staffIdx = hairpinSegment->vStaffIdx();
    std::vector<RectF> obstacles = collectChordAndFingeringRects(score, staffIdx);
    ASSERT_FALSE(obstacles.empty());

    const RectF beforeRect = hairpinSegmentSystemRect(hairpinSegment);
    bool initiallyOverlaps = false;
    RectF firstObstacle;
    RectF allObstacles;
    for (const RectF& obstacle : obstacles) {
        if (firstObstacle.isNull()) {
            firstObstacle = obstacle;
        }
        allObstacles.unite(obstacle);
        initiallyOverlaps = initiallyOverlaps || rectsOverlap(beforeRect, obstacle);
    }
    ASSERT_TRUE(initiallyOverlaps)
        << "hairpin before left=" << beforeRect.left() << " right=" << beforeRect.right()
        << " top=" << beforeRect.top() << " bottom=" << beforeRect.bottom()
        << " first obstacle left=" << firstObstacle.left() << " right=" << firstObstacle.right()
        << " top=" << firstObstacle.top() << " bottom=" << firstObstacle.bottom()
        << " all obstacles top=" << allObstacles.top() << " bottom=" << allObstacles.bottom();

    const mu::engraving::pm::PmPrettifyResult result = applyPrettifyCommand(score);
    EXPECT_TRUE(result.changed);

    hairpinSegments = collectLineHairpinSegments(score);
    ASSERT_EQ(hairpinSegments.size(), 1);
    hairpinSegment = hairpinSegments.front();
    obstacles = collectChordAndFingeringRects(score, hairpinSegment->vStaffIdx());
    const RectF afterRect = hairpinSegmentSystemRect(hairpinSegment);
    EXPECT_GT(afterRect.top(), beforeRect.top());
    for (const RectF& obstacle : obstacles) {
        EXPECT_FALSE(rectsOverlap(afterRect, obstacle))
            << "hairpin after left=" << afterRect.left() << " right=" << afterRect.right()
            << " top=" << afterRect.top() << " bottom=" << afterRect.bottom()
            << " obstacle left=" << obstacle.left() << " right=" << obstacle.right()
            << " top=" << obstacle.top() << " bottom=" << obstacle.bottom();
    }

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, autoLayoutAddsMissingKeyboardGrandStaffBraceOnce)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/two-keyboard-parts-no-brace.mscx");
    ASSERT_TRUE(score);
    ASSERT_EQ(score->nstaves(), 2);
    EXPECT_EQ(countBraceBracketsSpanning(score, 0, 2), 0);

    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    EXPECT_EQ(countBraceBracketsSpanning(score, 0, 2), 1);

    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    EXPECT_EQ(countBraceBracketsSpanning(score, 0, 2), 1);

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, autoLayoutHidesMetronomeTempoIndicatorsPreservingPlaybackTempo)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/tempo-indicators.mscx");
    ASSERT_TRUE(score);

    const std::vector<TempoText*> tempoTexts = collectTempoTexts(score);
    ASSERT_EQ(tempoTexts.size(), 4);
    const std::vector<StaffText*> staffTexts = collectStaffTexts(score);
    ASSERT_EQ(staffTexts.size(), 1);
    EXPECT_EQ(staffTexts.front()->plainText(), u"Allegretto");

    const BeatsPerSecond expressiveTempo = score->tempomap()->tempo(0);
    const BeatsPerSecond metronomeOnlyTempo = score->tempomap()->tempo(1920);
    const BeatsPerSecond expressiveRangeTempo = score->tempomap()->tempo(3840);
    const BeatsPerSecond circaMetronomeOnlyTempo = score->tempomap()->tempo(5760);

    mu::engraving::pm::applyPianomaniaAutoLayout(score);

    EXPECT_EQ(tempoTexts[0]->plainText(), u"Allegro");
    EXPECT_EQ(tempoTexts[0]->xmlText(), u"Allegro");
    EXPECT_FALSE(tempoTexts[0]->followText());
    EXPECT_TRUE(tempoTexts[0]->visible());
    EXPECT_FALSE(tempoTexts[1]->followText());
    EXPECT_FALSE(tempoTexts[1]->visible());
    EXPECT_TRUE(tempoTexts[1]->xmlText().isEmpty());
    EXPECT_EQ(tempoTexts[2]->plainText(), u"Allegro");
    EXPECT_EQ(tempoTexts[2]->xmlText(), u"Allegro");
    EXPECT_FALSE(tempoTexts[2]->followText());
    EXPECT_TRUE(tempoTexts[2]->visible());
    EXPECT_FALSE(tempoTexts[3]->followText());
    EXPECT_FALSE(tempoTexts[3]->visible());
    EXPECT_TRUE(tempoTexts[3]->xmlText().isEmpty());
    EXPECT_EQ(staffTexts.front()->plainText(), u"Allegretto");
    EXPECT_TRUE(muse::RealIsEqual(score->tempomap()->tempo(0).val, expressiveTempo.val));
    EXPECT_TRUE(muse::RealIsEqual(score->tempomap()->tempo(1920).val, metronomeOnlyTempo.val));
    EXPECT_TRUE(muse::RealIsEqual(score->tempomap()->tempo(3840).val, expressiveRangeTempo.val));
    EXPECT_TRUE(muse::RealIsEqual(score->tempomap()->tempo(5760).val, circaMetronomeOnlyTempo.val));

    const std::vector<TempoSnapshotEntry> firstSnapshot = captureTempoSnapshot(score);
    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    EXPECT_TRUE(tempoSnapshotsEquivalent(firstSnapshot, captureTempoSnapshot(score)));
    EXPECT_TRUE(muse::RealIsEqual(score->tempomap()->tempo(0).val, expressiveTempo.val));
    EXPECT_TRUE(muse::RealIsEqual(score->tempomap()->tempo(1920).val, metronomeOnlyTempo.val));
    EXPECT_TRUE(muse::RealIsEqual(score->tempomap()->tempo(3840).val, expressiveRangeTempo.val));
    EXPECT_TRUE(muse::RealIsEqual(score->tempomap()->tempo(5760).val, circaMetronomeOnlyTempo.val));

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, autoLayoutNormalizesExpressionStaffTextOnly)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/expression-staff-text.mscx");
    ASSERT_TRUE(score);

    const std::vector<StaffText*> staffTexts = collectStaffTexts(score);
    ASSERT_EQ(staffTexts.size(), 2);
    ASSERT_EQ(staffTexts[0]->plainText(), u"dolce");
    ASSERT_EQ(staffTexts[1]->plainText(), u"Allegretto");
    EXPECT_EQ(staffTexts[0]->textStyleType(), TextStyleType::STAFF);
    EXPECT_EQ(staffTexts[1]->textStyleType(), TextStyleType::STAFF);
    EXPECT_EQ(staffTexts[0]->propertyFlags(Pid::FONT_FACE), PropertyFlags::UNSTYLED);
    EXPECT_EQ(staffTexts[0]->propertyFlags(Pid::FONT_STYLE), PropertyFlags::UNSTYLED);
    EXPECT_EQ(staffTexts[0]->propertyFlags(Pid::FONT_SIZE), PropertyFlags::UNSTYLED);

    mu::engraving::pm::applyPianomaniaAutoLayout(score);

    EXPECT_EQ(staffTexts[0]->plainText(), u"dolce");
    EXPECT_EQ(staffTexts[0]->textStyleType(), TextStyleType::EXPRESSION);
    EXPECT_EQ(staffTexts[0]->propertyFlags(Pid::FONT_FACE), PropertyFlags::STYLED);
    EXPECT_EQ(staffTexts[0]->propertyFlags(Pid::FONT_STYLE), PropertyFlags::STYLED);
    EXPECT_EQ(staffTexts[0]->propertyFlags(Pid::FONT_SIZE), PropertyFlags::STYLED);

    EXPECT_EQ(staffTexts[1]->plainText(), u"Allegretto");
    EXPECT_EQ(staffTexts[1]->textStyleType(), TextStyleType::STAFF);

    const std::vector<StaffTextStyleSnapshotEntry> firstSnapshot = captureStaffTextStyleSnapshot(score);
    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    EXPECT_TRUE(staffTextStyleSnapshotsEquivalent(firstSnapshot, captureStaffTextStyleSnapshot(score)));

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, expressionClearsFollowingBarlineAndPersists)
{
    struct PrettifyFlagsOff {
        const bool previousPrettify = MScore::pianomaniaPrettifySlursFingerings;
        const bool previousForceNormalize = MScore::pianomaniaForceNormalizeSlursFingerings;

        PrettifyFlagsOff()
        {
            MScore::pianomaniaPrettifySlursFingerings = false;
            MScore::pianomaniaForceNormalizeSlursFingerings = false;
        }

        ~PrettifyFlagsOff()
        {
            MScore::pianomaniaPrettifySlursFingerings = previousPrettify;
            MScore::pianomaniaForceNormalizeSlursFingerings = previousForceNormalize;
        }
    } prettifyFlagsOff;

    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/expression-following-barline.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    Expression* expression = expressionWithText(score, u"poco ritenuto");
    ASSERT_TRUE(expression);
    ASSERT_TRUE(adjacentBarline(expression, false));
    ASSERT_TRUE(adjacentBarline(expression, true));

    const String originalText = expression->xmlText();
    const Fraction originalTick = expression->tick();
    const staff_idx_t originalStaff = expression->staffIdx();
    const PlacementV originalPlacement = expression->placement();
    const PointF originalOffset = expression->offset();
    const PropertyFlags originalOffsetFlags = expression->propertyFlags(Pid::OFFSET);
    EXPECT_EQ(originalOffsetFlags, PropertyFlags::STYLED);
    const double originalY = expression->pagePos().y();
    const double minimumClearance = 0.25 * expression->spatium();
    EXPECT_LT(followingBarlineClearance(expression), minimumClearance - 0.05);

    const mu::engraving::pm::PmPrettifyResult firstResult = applyPrettifyCommand(score);
    EXPECT_TRUE(firstResult.changed);
    EXPECT_FALSE(firstResult.structuralAssignmentChanged);
    EXPECT_GE(followingBarlineClearance(expression), minimumClearance - 0.05);
    EXPECT_GE(precedingBarlineClearance(expression), minimumClearance - 0.05);
    EXPECT_LT(expression->pagePos().x(), expression->segment()->pagePos().x());
    EXPECT_LE(expression->pageBoundingRect().left(), expression->segment()->pagePos().x());
    EXPECT_GE(expression->pageBoundingRect().right(), expression->segment()->pagePos().x());
    EXPECT_EQ(expression->xmlText(), originalText);
    EXPECT_EQ(expression->tick(), originalTick);
    EXPECT_EQ(expression->staffIdx(), originalStaff);
    EXPECT_EQ(expression->placement(), originalPlacement);
    EXPECT_TRUE(expression->autoplace());
    EXPECT_EQ(expression->propertyFlags(Pid::OFFSET), PropertyFlags::UNSTYLED);
    EXPECT_NEAR(expression->pagePos().y(), originalY, 0.05);

    const PointF repairedOffset = expression->offset();
    const PointF repairedPosition = expression->pagePos();
    relayoutScore(score);
    EXPECT_TRUE(pointNear(expression->pagePos(), repairedPosition, 0.02 * expression->spatium()));
    EXPECT_GE(followingBarlineClearance(expression), minimumClearance - 0.05);

    EditData undoEditData;
    score->undoStack()->undo(&undoEditData);
    relayoutScore(score);
    EXPECT_EQ(expression->offset(), originalOffset);
    EXPECT_EQ(expression->propertyFlags(Pid::OFFSET), originalOffsetFlags);

    EditData redoEditData;
    score->undoStack()->redo(&redoEditData);
    relayoutScore(score);
    EXPECT_EQ(expression->offset(), repairedOffset);
    EXPECT_EQ(expression->propertyFlags(Pid::OFFSET), PropertyFlags::UNSTYLED);
    EXPECT_TRUE(pointNear(expression->pagePos(), repairedPosition, 0.02 * expression->spatium()));
    EXPECT_GE(followingBarlineClearance(expression), minimumClearance - 0.05);

    applyPrettifyCommand(score);
    EXPECT_EQ(expression->offset(), repairedOffset);
    EXPECT_TRUE(pointNear(expression->pagePos(), repairedPosition, 0.02 * expression->spatium()));
    EXPECT_GE(followingBarlineClearance(expression), minimumClearance - 0.05);

    const std::string savedFileName = testing::TempDir() + "pianomania-expression-following-barline-roundtrip.mscx";
    const String savedPath = String::fromUtf8(savedFileName);
    ASSERT_TRUE(ScoreRW::saveScore(score, savedPath));
    MasterScore* reloadedScore = ScoreRW::readScore(savedPath, true);
    ASSERT_TRUE(reloadedScore);
    Expression* reloadedExpression = expressionWithText(reloadedScore, u"poco ritenuto");
    ASSERT_TRUE(reloadedExpression);
    relayoutScore(reloadedScore);

    EXPECT_TRUE(pointNear(reloadedExpression->offset(), repairedOffset, 0.02 * reloadedExpression->spatium()));
    EXPECT_EQ(reloadedExpression->propertyFlags(Pid::OFFSET), PropertyFlags::UNSTYLED);
    EXPECT_GE(followingBarlineClearance(reloadedExpression), 0.25 * reloadedExpression->spatium() - 0.05);
    EXPECT_GE(precedingBarlineClearance(reloadedExpression), 0.25 * reloadedExpression->spatium() - 0.05);
    EXPECT_LE(reloadedExpression->pageBoundingRect().left(), reloadedExpression->segment()->pagePos().x());
    EXPECT_GE(reloadedExpression->pageBoundingRect().right(), reloadedExpression->segment()->pagePos().x());
    EXPECT_EQ(reloadedExpression->xmlText(), originalText);
    EXPECT_EQ(reloadedExpression->tick(), originalTick);
    EXPECT_EQ(reloadedExpression->staffIdx(), originalStaff);
    EXPECT_EQ(reloadedExpression->placement(), originalPlacement);
    EXPECT_TRUE(reloadedExpression->autoplace());
    EXPECT_NEAR(reloadedExpression->pagePos().y(), originalY, 0.05);

    const PointF reloadedOffset = reloadedExpression->offset();
    const PointF reloadedPosition = reloadedExpression->pagePos();
    applyPrettifyCommand(reloadedScore);
    relayoutScore(reloadedScore);
    EXPECT_TRUE(pointNear(reloadedExpression->offset(), reloadedOffset, 0.02 * reloadedExpression->spatium()));
    EXPECT_TRUE(pointNear(reloadedExpression->pagePos(), reloadedPosition, 0.02 * reloadedExpression->spatium()));
    EXPECT_GE(followingBarlineClearance(reloadedExpression), 0.25 * reloadedExpression->spatium() - 0.05);

    delete reloadedScore;
    std::remove(savedFileName.c_str());
    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, expressionBarlineClearancePreservesManualPlacement)
{
    for (int scenario = 0; scenario < 2; ++scenario) {
        SCOPED_TRACE(scenario);
        MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/expression-following-barline.mscx");
        ASSERT_TRUE(score);
        Expression* expression = expressionWithText(score, u"poco ritenuto");
        ASSERT_TRUE(expression);

        if (scenario == 0) {
            expression->setProperty(Pid::OFFSET, PointF(0.4 * expression->spatium(), 0.0));
            expression->setPropertyFlags(Pid::OFFSET, PropertyFlags::UNSTYLED);
        } else {
            expression->setProperty(Pid::AUTOPLACE, false);
            expression->setPropertyFlags(Pid::AUTOPLACE, PropertyFlags::UNSTYLED);
        }
        relayoutScore(score);
        const PointF manualOffset = expression->offset();
        const PropertyFlags manualOffsetFlags = expression->propertyFlags(Pid::OFFSET);
        const bool manualAutoplace = expression->autoplace();
        const PropertyFlags manualAutoplaceFlags = expression->propertyFlags(Pid::AUTOPLACE);
        const PointF manualPosition = expression->pagePos();

        applyPrettifyCommand(score);
        EXPECT_EQ(expression->offset(), manualOffset);
        EXPECT_EQ(expression->propertyFlags(Pid::OFFSET), manualOffsetFlags);
        EXPECT_EQ(expression->autoplace(), manualAutoplace);
        EXPECT_EQ(expression->propertyFlags(Pid::AUTOPLACE), manualAutoplaceFlags);
        EXPECT_TRUE(pointNear(expression->pagePos(), manualPosition, 0.02 * expression->spatium()));

        delete score;
    }
}

TEST_F(Engraving_PianomaniaPrettifyTests, slurEndpointOffsetUndoRedoClearsStaleEndpointCarryover)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/prettify-idempotency.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    SlurSegment* segment = firstSlurSegment(score);
    ASSERT_TRUE(segment);

    const PointF originalStartOffset = segment->ups(Grip::START).off;
    const double spatium = std::max(1.0, segment->spatium());
    const PointF staleEndpointCarryover(0.41 * spatium, 0.29 * spatium);
    const PointF changedStartOffset = originalStartOffset + PointF(0.7 * spatium, -0.2 * spatium);

    segment->setEndPointOff1(staleEndpointCarryover);

    score->startCmd(TranslatableString::untranslatable("slur endpoint offset carryover test"));
    segment->undoChangeProperty(Pid::SLUR_UOFF1, changedStartOffset, segment->propertyFlags(Pid::SLUR_UOFF1));
    score->endCmd(false);
    relayoutScore(score);

    EXPECT_TRUE(pointNear(segment->ups(Grip::START).off, changedStartOffset, 0.02 * spatium));
    EXPECT_TRUE(pointNear(segment->endPointOff1(), PointF(), 0.02 * spatium));

    EditData undoEditData;
    score->undoStack()->undo(&undoEditData);
    relayoutScore(score);
    EXPECT_TRUE(pointNear(segment->ups(Grip::START).off, originalStartOffset, 0.02 * spatium));

    segment->setEndPointOff1(staleEndpointCarryover);

    EditData redoEditData;
    score->undoStack()->redo(&redoEditData);
    relayoutScore(score);
    EXPECT_TRUE(pointNear(segment->ups(Grip::START).off, changedStartOffset, 0.02 * spatium));
    EXPECT_TRUE(pointNear(segment->endPointOff1(), PointF(), 0.02 * spatium));

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, autoLayoutResetsStaleManualPlacement)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/manual-placement-normalization.mscx");
    ASSERT_TRUE(score);

    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    relayoutScore(score);

    for (Dynamic* dynamic : collectAnnotations<Dynamic>(score, &EngravingObject::isDynamic)) {
        EXPECT_TRUE(dynamic->autoplace());
        EXPECT_EQ(dynamic->offset(), dynamic->propertyDefault(Pid::OFFSET).value<PointF>());
        EXPECT_FALSE(dynamic->placeAbove());
    }

    // Offsets are layout-relative and reset; an authored side stays.
    const std::vector<Hairpin*> hairpins = collectHairpins(score);
    ASSERT_EQ(hairpins.size(), 2);
    for (Hairpin* hairpin : hairpins) {
        const bool forcedUp = hairpin->getProperty(Pid::DIRECTION).value<DirectionV>() == DirectionV::UP;
        EXPECT_EQ(forcedUp, hairpin->beginText().isEmpty());
        ASSERT_FALSE(hairpin->spannerSegments().empty());
        for (SpannerSegment* segment : hairpin->spannerSegments()) {
            EXPECT_TRUE(segment->autoplace());
            EXPECT_TRUE(segment->offset().isNull() || segment->isStyled(Pid::OFFSET));
            EXPECT_EQ(segment->placeAbove(), forcedUp);
        }
    }

    for (Fermata* fermata : collectAnnotations<Fermata>(score, &EngravingObject::isFermata)) {
        EXPECT_TRUE(fermata->autoplace());
        EXPECT_TRUE(fermata->offset().isNull() || fermata->isStyled(Pid::OFFSET));
    }

    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(SegmentType::ChordRest); segment; segment = segment->next(SegmentType::ChordRest)) {
            for (EngravingItem* item : segment->elist()) {
                if (!item || !item->isChord()) {
                    continue;
                }
                for (Articulation* articulation : toChord(item)->articulations()) {
                    EXPECT_TRUE(articulation->autoplace());
                    EXPECT_TRUE(articulation->offset().isNull());
                }
            }
        }
    }

    delete score;
}

// Burgmuller Op. 100 No. 2 m24 stores "dolce" on the B after an opening
// eighth rest. Its only above-staff instruction is the effective position of
// a manual offset; resetting that offset must retain the authored side. The
// fixture is the exact corrected source (SHA-256 6b9e3c8b8d092b3877dc004a9f9b663a1009c2de27108d98c86429a03cd1808a),
// because a numeric offset has meaning only in the layout that authored it.
// An ordinary default expression must keep automatic below placement.
TEST_F(Engraving_PianomaniaPrettifyTests, autoLayoutRetainsOuterStaffSideOfManuallyPlacedExpression)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/burgmuller-op100-no2-text-placement.mscz");
    ASSERT_TRUE(score);
    relayoutScore(score);

    Expression* dolce = expressionWithText(score, u"dolce");
    ASSERT_TRUE(dolce);

    Segment* ordinaryTarget = dolce->segment()->next1(SegmentType::ChordRest);
    while (ordinaryTarget && (!ordinaryTarget->element(0) || !ordinaryTarget->element(0)->isChord())) {
        ordinaryTarget = ordinaryTarget->next1(SegmentType::ChordRest);
    }
    ASSERT_TRUE(ordinaryTarget);

    Expression* ordinary = Factory::createExpression(ordinaryTarget);
    ordinary->setTrack(0);
    ordinary->setXmlText(u"cantabile");
    ordinaryTarget->add(ordinary);
    relayoutScore(score);

    const double spatium = dolce->spatium();
    const System* authoredSystem = dolce->segment()->system();
    ASSERT_TRUE(authoredSystem);
    const double authoredStaffTop = authoredSystem->pagePos().y() + authoredSystem->staff(dolce->staffIdx())->y();
    EXPECT_NEAR(dolce->offset().x() / spatium, -1.45503, 1e-4);
    EXPECT_NEAR(dolce->offset().y() / spatium, -6.03662, 1e-4);
    EXPECT_DOUBLE_EQ(dolce->minDistance().val(), -999.0);
    ASSERT_LE(dolce->pageBoundingRect().bottom(), authoredStaffTop);
    ASSERT_FALSE(dolce->placeAbove());
    ASSERT_FALSE(ordinary->placeAbove());
    ASSERT_EQ(ordinary->offset(), ordinary->propertyDefault(Pid::OFFSET).value<PointF>());

    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    relayoutScore(score);

    EXPECT_TRUE(dolce->placeAbove());
    EXPECT_EQ(dolce->getProperty(Pid::DIRECTION).value<DirectionV>(), DirectionV::UP);
    EXPECT_EQ(dolce->propertyFlags(Pid::DIRECTION), PropertyFlags::NOSTYLE);
    EXPECT_EQ(dolce->offset(), dolce->propertyDefault(Pid::OFFSET).value<PointF>());
    EXPECT_DOUBLE_EQ(dolce->minDistance().val(),
                     dolce->propertyDefault(Pid::MIN_DISTANCE).value<Spatium>().val());
    const System* laidOutSystem = dolce->segment()->system();
    ASSERT_TRUE(laidOutSystem);
    const double laidOutStaffTop = laidOutSystem->pagePos().y() + laidOutSystem->staff(dolce->staffIdx())->y();
    EXPECT_LT(dolce->pageBoundingRect().bottom(), laidOutStaffTop);

    EXPECT_FALSE(ordinary->placeAbove());
    EXPECT_EQ(ordinary->offset(), ordinary->propertyDefault(Pid::OFFSET).value<PointF>());

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, autoLayoutPlacesLowerStaffFermataBelow)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/manual-placement-normalization.mscx");
    ASSERT_TRUE(score);

    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    relayoutScore(score);

    const std::vector<Fermata*> fermatas = collectAnnotations<Fermata>(score, &EngravingObject::isFermata);
    ASSERT_EQ(fermatas.size(), 2);
    for (Fermata* fermata : fermatas) {
        const System* system = fermata->segment()->system();
        const double staffTop = system->pagePos().y() + system->staff(fermata->staffIdx())->y();
        const double staffBottom = staffTop + fermata->staff()->staffHeight(fermata->tick());
        if (fermata->staffIdx() == 0) {
            EXPECT_TRUE(fermata->placeAbove());
            EXPECT_LT(fermata->pageBoundingRect().bottom(), staffTop);
        } else {
            EXPECT_FALSE(fermata->placeAbove());
            EXPECT_GT(fermata->pageBoundingRect().top(), staffBottom);
        }
    }

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, prettifyCompactsAutomaticFermataBesideCompoundFingeringStack)
{
    struct StackCase {
        Fermata* fermata = nullptr;
        Chord* chord = nullptr;
        std::vector<Fingering*> fingerings;
    };

    auto stackCase = [](Score* current, int measureIndex, staff_idx_t staffIdx) {
        StackCase result;
        Measure* measure = current->firstMeasure();
        for (int i = 0; measure && i < measureIndex; ++i) {
            measure = measure->nextMeasure();
        }
        if (!measure) {
            return result;
        }
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            if (!segment->isChordRestType()) {
                continue;
            }
            for (EngravingItem* annotation : segment->annotations()) {
                if (annotation && annotation->isFermata() && annotation->vStaffIdx() == staffIdx) {
                    result.fermata = toFermata(annotation);
                }
            }
            for (EngravingItem* item : segment->elist()) {
                if (!item || !item->isChord() || item->vStaffIdx() != staffIdx) {
                    continue;
                }
                result.chord = toChord(item);
                for (Note* note : result.chord->notes()) {
                    for (EngravingItem* noteItem : note->el()) {
                        if (noteItem && noteItem->isFingering()) {
                            result.fingerings.push_back(toFingering(noteItem));
                        }
                    }
                }
            }
        }
        return result;
    };
    auto groupRect = [](const StackCase& value) {
        RectF result;
        for (const Fingering* fingering : value.fingerings) {
            result.unite(fingering->pageBoundingRect());
        }
        return result;
    };
    auto pitches = [](const StackCase& value) {
        std::vector<int> result;
        for (const Fingering* fingering : value.fingerings) {
            result.push_back(fingering->note()->pitch());
        }
        return result;
    };
    auto positions = [](const StackCase& value) {
        std::vector<PointF> result;
        for (const Fingering* fingering : value.fingerings) {
            result.push_back(fingering->pos());
        }
        return result;
    };
    auto offsets = [](const StackCase& value) {
        std::vector<PointF> result;
        for (const Fingering* fingering : value.fingerings) {
            result.push_back(fingering->offset());
        }
        return result;
    };
    auto offsetFlags = [](const StackCase& value) {
        std::vector<PropertyFlags> result;
        for (const Fingering* fingering : value.fingerings) {
            result.push_back(fingering->propertyFlags(Pid::OFFSET));
        }
        return result;
    };
    auto expectPositionsNear = [](const StackCase& value, const std::vector<PointF>& expected) {
        ASSERT_EQ(value.fingerings.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            const double tolerance = 0.02 * value.fingerings[i]->spatium();
            EXPECT_NEAR(value.fingerings[i]->pos().x(), expected[i].x(), tolerance);
            EXPECT_NEAR(value.fingerings[i]->pos().y(), expected[i].y(), tolerance);
        }
    };
    auto eids = [](const StackCase& value) {
        std::vector<EID> result;
        result.push_back(value.fermata->eid());
        result.push_back(value.chord->eid());
        for (const Fingering* fingering : value.fingerings) {
            result.push_back(fingering->eid());
            result.push_back(fingering->note()->eid());
        }
        return result;
    };
    auto ensureEids = [](const StackCase& value) {
        std::vector<EngravingObject*> objects { value.fermata, value.chord };
        for (Fingering* fingering : value.fingerings) {
            objects.push_back(fingering);
            objects.push_back(fingering->note());
        }
        for (EngravingObject* object : objects) {
            if (object && !object->eid().isValid()) {
                object->assignNewEID();
            }
        }
    };
    auto expectSidePocket = [&](const StackCase& value, bool above) {
        ASSERT_TRUE(value.fermata);
        ASSERT_TRUE(value.chord);
        ASSERT_GE(value.fingerings.size(), 2);
        const RectF digits = groupRect(value);
        const RectF fermata = value.fermata->pageBoundingRect();
        const double clearance = 0.25 * value.fermata->spatium();
        EXPECT_TRUE(digits.right() <= fermata.left() - clearance + 0.02
                    || digits.left() >= fermata.right() + clearance - 0.02);
        EXPECT_EQ(value.fermata->placeAbove(), above);
        EXPECT_TRUE(value.fermata->autoplace());
        EXPECT_TRUE(value.fermata->isStyled(Pid::OFFSET));
        const System* system = value.fermata->segment()->system();
        ASSERT_TRUE(system);
        const double staffTop = system->pagePos().y() + system->staff(value.fermata->vStaffIdx())->y();
        const double staffBottom = staffTop + value.fermata->staff()->staffHeight(value.fermata->tick());
        const double staffGap = above ? staffTop - fermata.bottom() : fermata.top() - staffBottom;
        EXPECT_GE(staffGap, 0.25 * value.fermata->spatium() - 0.02);
        EXPECT_LE(staffGap, 1.0 * value.fermata->spatium() + 0.02);
        for (const Fingering* fingering : value.fingerings) {
            EXPECT_EQ(fingering->placement() == PlacementV::ABOVE, above);
        }
        const double horizontalAssociation = std::abs(digits.center().x() - value.chord->pageBoundingRect().center().x());
        EXPECT_LE(horizontalAssociation, 3.0 * value.fermata->spatium() + 0.02);
    };

    const String fixture = u"pianomania_prettify_data/fingering-fermata-compound-stack.mscx";
    MasterScore* score = ScoreRW::readScore(fixture);
    ASSERT_TRUE(score);
    relayoutScore(score);

    StackCase upper = stackCase(score, 0, 0);
    StackCase lower = stackCase(score, 0, 1);
    StackCase upperSingle = stackCase(score, 1, 0);
    StackCase lowerSingle = stackCase(score, 1, 1);
    StackCase upperManual = stackCase(score, 2, 0);
    StackCase lowerManual = stackCase(score, 2, 1);
    StackCase upperOffset = stackCase(score, 3, 0);
    StackCase lowerOffset = stackCase(score, 3, 1);
    StackCase blocked = stackCase(score, 4, 0);
    ASSERT_TRUE(upper.fermata);
    ASSERT_TRUE(lower.fermata);
    ASSERT_EQ(upper.fingerings.size(), 3);
    ASSERT_EQ(lower.fingerings.size(), 3);
    ASSERT_EQ(upperSingle.fingerings.size(), 1);
    ASSERT_EQ(lowerSingle.fingerings.size(), 1);
    ASSERT_EQ(blocked.fingerings.size(), 3);
    ensureEids(upper);
    ensureEids(lower);

    const RectF originalUpperFermata = upper.fermata->pageBoundingRect();
    const RectF originalLowerFermata = lower.fermata->pageBoundingRect();
    const PointF originalUpperSingle = upperSingle.fingerings.front()->pos();
    const PointF originalLowerSingle = lowerSingle.fingerings.front()->pos();
    const PointF originalUpperManualOffset = upperManual.fermata->offset();
    const PointF originalLowerManualOffset = lowerManual.fermata->offset();
    const PointF originalUpperManualPosition = upperManual.fermata->pos();
    const PointF originalLowerManualPosition = lowerManual.fermata->pos();
    const PointF originalUpperOffset = upperOffset.fermata->offset();
    const PointF originalLowerOffset = lowerOffset.fermata->offset();
    const PointF originalUpperOffsetPosition = upperOffset.fermata->pos();
    const PointF originalLowerOffsetPosition = lowerOffset.fermata->pos();
    const PointF originalUpperFermataPosition = upper.fermata->pos();
    const PointF originalLowerFermataPosition = lower.fermata->pos();
    const std::vector<PointF> originalUpperPositions = positions(upper);
    const std::vector<PointF> originalLowerPositions = positions(lower);
    const std::vector<PointF> originalUpperOffsets = offsets(upper);
    const std::vector<PointF> originalLowerOffsets = offsets(lower);
    const std::vector<PropertyFlags> originalUpperOffsetFlags = offsetFlags(upper);
    const std::vector<PropertyFlags> originalLowerOffsetFlags = offsetFlags(lower);
    const bool originalUpperOffsetStyled = upper.fermata->isStyled(Pid::OFFSET);
    const bool originalLowerOffsetStyled = lower.fermata->isStyled(Pid::OFFSET);
    const PointF originalBlockedFermataPosition = blocked.fermata->pos();
    const std::vector<PointF> originalBlockedPositions = positions(blocked);
    const std::vector<int> upperPitches = pitches(upper);
    const std::vector<int> lowerPitches = pitches(lower);
    const std::vector<EID> upperEids = eids(upper);
    const std::vector<EID> lowerEids = eids(lower);

    const auto firstResult = applyPrettifyCommand(score);
    EXPECT_TRUE(firstResult.changed);
    upper = stackCase(score, 0, 0);
    lower = stackCase(score, 0, 1);
    upperSingle = stackCase(score, 1, 0);
    lowerSingle = stackCase(score, 1, 1);
    upperManual = stackCase(score, 2, 0);
    lowerManual = stackCase(score, 2, 1);
    upperOffset = stackCase(score, 3, 0);
    lowerOffset = stackCase(score, 3, 1);
    blocked = stackCase(score, 4, 0);
    expectSidePocket(upper, true);
    expectSidePocket(lower, false);
    EXPECT_GT(upper.fermata->pageBoundingRect().bottom(), originalUpperFermata.bottom() + upper.fermata->spatium());
    EXPECT_LT(lower.fermata->pageBoundingRect().top(), originalLowerFermata.top() - lower.fermata->spatium());
    EXPECT_TRUE(pointNear(upperSingle.fingerings.front()->pos(), originalUpperSingle,
                          0.02 * upperSingle.fingerings.front()->spatium()));
    EXPECT_TRUE(pointNear(lowerSingle.fingerings.front()->pos(), originalLowerSingle,
                          0.02 * lowerSingle.fingerings.front()->spatium()));
    EXPECT_EQ(upperManual.fermata->offset(), originalUpperManualOffset);
    EXPECT_EQ(lowerManual.fermata->offset(), originalLowerManualOffset);
    EXPECT_FALSE(upperManual.fermata->autoplace());
    EXPECT_FALSE(lowerManual.fermata->autoplace());
    EXPECT_TRUE(pointNear(upperManual.fermata->pos(), originalUpperManualPosition,
                          0.02 * upperManual.fermata->spatium()));
    EXPECT_TRUE(pointNear(lowerManual.fermata->pos(), originalLowerManualPosition,
                          0.02 * lowerManual.fermata->spatium()));
    EXPECT_TRUE(upperOffset.fermata->autoplace());
    EXPECT_TRUE(lowerOffset.fermata->autoplace());
    EXPECT_FALSE(upperOffset.fermata->isStyled(Pid::OFFSET));
    EXPECT_FALSE(lowerOffset.fermata->isStyled(Pid::OFFSET));
    EXPECT_EQ(upperOffset.fermata->offset(), originalUpperOffset);
    EXPECT_EQ(lowerOffset.fermata->offset(), originalLowerOffset);
    EXPECT_TRUE(pointNear(upperOffset.fermata->pos(), originalUpperOffsetPosition,
                          0.02 * upperOffset.fermata->spatium()));
    EXPECT_TRUE(pointNear(lowerOffset.fermata->pos(), originalLowerOffsetPosition,
                          0.02 * lowerOffset.fermata->spatium()));
    for (size_t i = 0; i < blocked.fingerings.size(); ++i) {
        EXPECT_NEAR(blocked.fingerings[i]->pos().x(), originalBlockedPositions[i].x(),
                    0.02 * blocked.fingerings[i]->spatium());
    }
    // The final 32nd-note chord is bounded by the preceding stem/note and the
    // real end barline/page edge. No horizontal candidate is safe, so retain
    // the ordinary centered, collision-free vertical stack.
    const RectF blockedDigits = groupRect(blocked);
    const RectF blockedFermata = blocked.fermata->pageBoundingRect();
    EXPECT_LT(blockedDigits.left(), blockedFermata.right());
    EXPECT_GT(blockedDigits.right(), blockedFermata.left());
    EXPECT_TRUE(blocked.fermata->autoplace());
    const Page* blockedPage = blocked.fermata->page();
    ASSERT_TRUE(blockedPage);
    for (const Fingering* fingering : blocked.fingerings) {
        const RectF pageRect = fingering->pageBoundingRect();
        EXPECT_GE(pageRect.left(), 0.0);
        EXPECT_GE(pageRect.top(), 0.0);
        EXPECT_LE(pageRect.right(), blockedPage->width());
        EXPECT_LE(pageRect.bottom(), blockedPage->height());
    }
    EXPECT_EQ(pitches(upper), upperPitches);
    EXPECT_EQ(pitches(lower), lowerPitches);
    EXPECT_EQ(eids(upper), upperEids);
    EXPECT_EQ(eids(lower), lowerEids);
    const std::vector<PointF> acceptedBlockedPositions = positions(blocked);
    const PointF acceptedBlockedFermataPosition = blocked.fermata->pos();
    const std::vector<PointF> acceptedUpperOffsets = offsets(upper);
    const std::vector<PointF> acceptedLowerOffsets = offsets(lower);
    const std::vector<PropertyFlags> acceptedUpperOffsetFlags = offsetFlags(upper);
    const std::vector<PropertyFlags> acceptedLowerOffsetFlags = offsetFlags(lower);

    const auto repeated = applyPrettifyCommand(score);
    EXPECT_FALSE(repeated.changed);
    upper = stackCase(score, 0, 0);
    lower = stackCase(score, 0, 1);
    blocked = stackCase(score, 4, 0);
    expectSidePocket(upper, true);
    expectSidePocket(lower, false);
    expectPositionsNear(blocked, acceptedBlockedPositions);
    EXPECT_TRUE(pointNear(blocked.fermata->pos(), acceptedBlockedFermataPosition,
                          0.02 * blocked.fermata->spatium()));

    EditData undoEditData;
    score->undoStack()->undo(&undoEditData);
    relayoutScore(score);
    upper = stackCase(score, 0, 0);
    lower = stackCase(score, 0, 1);
    blocked = stackCase(score, 4, 0);
    expectPositionsNear(upper, originalUpperPositions);
    expectPositionsNear(lower, originalLowerPositions);
    EXPECT_TRUE(pointNear(upper.fermata->pos(), originalUpperFermataPosition,
                          0.02 * upper.fermata->spatium()));
    EXPECT_TRUE(pointNear(lower.fermata->pos(), originalLowerFermataPosition,
                          0.02 * lower.fermata->spatium()));
    EXPECT_EQ(upper.fermata->isStyled(Pid::OFFSET), originalUpperOffsetStyled);
    EXPECT_EQ(lower.fermata->isStyled(Pid::OFFSET), originalLowerOffsetStyled);
    EXPECT_EQ(offsets(upper), originalUpperOffsets);
    EXPECT_EQ(offsets(lower), originalLowerOffsets);
    EXPECT_EQ(offsetFlags(upper), originalUpperOffsetFlags);
    EXPECT_EQ(offsetFlags(lower), originalLowerOffsetFlags);
    expectPositionsNear(blocked, originalBlockedPositions);
    EXPECT_TRUE(pointNear(blocked.fermata->pos(), originalBlockedFermataPosition,
                          0.02 * blocked.fermata->spatium()));

    EditData redoEditData;
    score->undoStack()->redo(&redoEditData);
    relayoutScore(score);
    upper = stackCase(score, 0, 0);
    lower = stackCase(score, 0, 1);
    blocked = stackCase(score, 4, 0);
    expectSidePocket(upper, true);
    expectSidePocket(lower, false);
    expectPositionsNear(blocked, acceptedBlockedPositions);
    EXPECT_TRUE(pointNear(blocked.fermata->pos(), acceptedBlockedFermataPosition,
                          0.02 * blocked.fermata->spatium()));
    EXPECT_EQ(offsets(upper), acceptedUpperOffsets);
    EXPECT_EQ(offsets(lower), acceptedLowerOffsets);
    EXPECT_EQ(offsetFlags(upper), acceptedUpperOffsetFlags);
    EXPECT_EQ(offsetFlags(lower), acceptedLowerOffsetFlags);

    const std::string savedFileName = testing::TempDir() + "fingering-fermata-compound-stack-roundtrip.mscx";
    const String savedPath = String::fromUtf8(savedFileName);
    ASSERT_TRUE(ScoreRW::saveScore(score, savedPath));
    MasterScore* reloaded = ScoreRW::readScore(savedPath, true);
    ASSERT_TRUE(reloaded);
    relayoutScore(reloaded);
    StackCase reloadedUpper = stackCase(reloaded, 0, 0);
    StackCase reloadedLower = stackCase(reloaded, 0, 1);
    expectSidePocket(reloadedUpper, true);
    expectSidePocket(reloadedLower, false);
    EXPECT_EQ(pitches(reloadedUpper), upperPitches);
    EXPECT_EQ(pitches(reloadedLower), lowerPitches);
    EXPECT_EQ(eids(reloadedUpper), upperEids);
    EXPECT_EQ(eids(reloadedLower), lowerEids);
    EXPECT_FALSE(applyPrettifyCommand(reloaded).changed);

    delete reloaded;
    std::remove(savedFileName.c_str());
    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, autoLayoutSplitsGlyphDynamicIntoDynamicAndExpression)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/manual-placement-normalization.mscx");
    ASSERT_TRUE(score);

    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    relayoutScore(score);

    Segment* firstBeat = score->firstMeasure()->first(SegmentType::ChordRest);
    Dynamic* dynamic = nullptr;
    std::vector<Expression*> expressions;
    for (EngravingItem* item : firstBeat->annotations()) {
        if (item->isDynamic()) {
            dynamic = toDynamic(item);
        } else if (item->isExpression()) {
            expressions.push_back(toExpression(item));
        }
    }
    ASSERT_TRUE(dynamic);
    EXPECT_EQ(dynamic->dynamicType(), DynamicType::P);
    EXPECT_EQ(dynamic->xmlText(), Dynamic::dynamicText(DynamicType::P));
    EXPECT_EQ(dynamic->velocity(), 50);
    ASSERT_EQ(expressions.size(), 1);
    EXPECT_EQ(expressions.front()->plainText(), u"religioso");
    EXPECT_EQ(expressions.front()->track(), dynamic->track());
    EXPECT_LT(dynamic->pageBoundingRect().right(), expressions.front()->pageBoundingRect().left());

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, autoLayoutWidensMeasuresSoLineTextClearsNextDynamic)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/manual-placement-normalization.mscx");
    ASSERT_TRUE(score);

    mu::engraving::pm::applyPianomaniaAutoLayout(score);

    // Natural widths are the tightest the casting can make them; justification only adds room.
    score->setLayoutMode(LayoutMode::LINE);
    relayoutScore(score);

    Hairpin* hairpin = hairpinWithBeginText(score);
    ASSERT_TRUE(hairpin);
    const auto* segment = static_cast<const TextLineBaseSegment*>(hairpin->frontSegment());
    ASSERT_TRUE(segment && segment->text());
    const std::vector<Dynamic*> dynamics = collectAnnotations<Dynamic>(score, &EngravingObject::isDynamic);
    const auto pp = std::find_if(dynamics.begin(), dynamics.end(), [](const Dynamic* d) {
        return d->dynamicType() == DynamicType::PP;
    });
    ASSERT_NE(pp, dynamics.end());

    EXPECT_GT(measureAt(score, 1)->userStretch(), 1.0);
    EXPECT_LE(segment->text()->pageBoundingRect().right(), (*pp)->pageBoundingRect().left());

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, restOfLowerVoiceEnteredAsVoiceOneStaysBelowUpperVoice)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/manual-placement-normalization.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);

    Measure* measure = measureAt(score, 3);
    ASSERT_TRUE(measure);
    Segment* firstBeat = measure->first(SegmentType::ChordRest);
    EngravingItem* restItem = firstBeat->element(0);
    EngravingItem* upperItem = firstBeat->element(1);
    ASSERT_TRUE(restItem && restItem->isRest());
    ASSERT_TRUE(upperItem && upperItem->isChord());

    const Rest* rest = toRest(restItem);
    const Note* upperNote = toChord(upperItem)->upNote();
    const double staffTop = measure->system()->pagePos().y() + measure->system()->staff(0)->y();
    EXPECT_GT(rest->pageBoundingRect().center().y(), upperNote->pageBoundingRect().center().y());
    EXPECT_GE(rest->pageBoundingRect().top(), staffTop);

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, rightHandFingeringStaysAboveWhenSlurLiftsIt)
{
    // The export pipeline keeps the prettify passes on through its final
    // layout (force-normalize), which is where a detached group could flip.
    struct ExportPipelineFlags {
        ExportPipelineFlags()
        {
            MScore::pianomaniaPrettifySlursFingerings = true;
            MScore::pianomaniaForceNormalizeSlursFingerings = true;
        }

        ~ExportPipelineFlags()
        {
            MScore::pianomaniaPrettifySlursFingerings = false;
            MScore::pianomaniaForceNormalizeSlursFingerings = false;
        }
    } exportPipelineFlags;

    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-right-hand-stays-above.mscx");
    ASSERT_TRUE(score);
    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    applyPrettifyCommand(score);

    for (const String& text : { String(u"1"), String(u"2"), String(u"3"), String(u"4"), String(u"5") }) {
        for (Fingering* fingering : collectFingeringsByText(score, text)) {
            EXPECT_EQ(fingering->placement(), PlacementV::ABOVE) << text.toStdString();
        }
    }

    delete score;
}

TEST_F(Engraving_PianomaniaPrettifyTests, stemSideAccentClearsSlurTakingOffAtItsStem)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/accent-slur-stem-side.mscx");
    ASSERT_TRUE(score);
    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    applyPrettifyCommand(score);

    std::vector<Chord*> accentedChords;
    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(SegmentType::ChordRest); segment;
             segment = segment->next(SegmentType::ChordRest)) {
            for (EngravingItem* item : segment->elist()) {
                if (item && item->isChord() && !toChord(item)->articulations().empty()) {
                    accentedChords.push_back(toChord(item));
                }
            }
        }
    }
    ASSERT_EQ(accentedChords.size(), 5);

    bool sawAbove = false;
    bool sawBelow = false;
    bool sawMarcato = false;
    bool sawStaccatissimo = false;
    for (const Chord* accented : accentedChords) {
        const Articulation* accent = accented->articulations().front();
        SlurSegment* slurSegment = nullptr;
        for (const auto& pair : score->spanner()) {
            Spanner* spanner = pair.second;
            if (spanner && spanner->isSlur() && toSlur(spanner)->startCR() == accented) {
                slurSegment = toSlur(spanner)->segmentAt(0);
                break;
            }
        }
        ASSERT_TRUE(slurSegment);

        const Segment* segment = accented->segment();
        const Measure* measure = accented->measure();
        ASSERT_TRUE(segment);
        ASSERT_TRUE(measure);
        ASSERT_TRUE(measure->system());
        const double accentStaffY = measure->system()->staff(accented->vStaffIdx())->y();
        const double slurStaffY = measure->system()->staff(slurSegment->vStaffIdx())->y();
        const double spatium = accent->spatium();
        const PointF accentOffset = accent->pos() + accented->pos() + segment->pos() + measure->pos()
                                    + accented->staffOffset() + PointF(0.0, accentStaffY);
        Shape accentShape(Shape::Type::Composite);
        accentShape.add(accent->shape().translated(accentOffset));
        accentShape.add(accent->ldata()->bbox().translated(accentOffset));
        const PointF slurOffset = slurSegment->pos() + PointF(0.0, slurStaffY);
        Shape slurShape = slurSegment->shape().translated(slurOffset);
        slurShape.add(sampledPathShape(slurSegment->ldata()->path(), slurOffset));
        const double margin = 0.4 * spatium;
        const double clearance = accent->up() ? accentShape.verticalClearance(slurShape, margin)
                                 : slurShape.verticalClearance(accentShape, margin);
        EXPECT_GE(clearance, 0.3 * spatium);
        EXPECT_GE(minimumShapeDistance(accentShape, slurShape), margin - 1e-4);
        sawAbove = sawAbove || accent->up();
        sawBelow = sawBelow || !accent->up();
        sawMarcato = sawMarcato || accent->isMarcato();
        sawStaccatissimo = sawStaccatissimo || accent->symId() == SymId::articStaccatissimoWedgeAbove;
    }
    EXPECT_TRUE(sawAbove);
    EXPECT_TRUE(sawBelow);
    EXPECT_TRUE(sawMarcato);
    EXPECT_TRUE(sawStaccatissimo);

    delete score;
}

// Test value: Keeps an unsnapped automatic wedge on the final staff in a
// readable upper lane of a compact grand staff while preserving the ordinary
// centered and authored-manual layout paths.
TEST_F(Engraving_PianomaniaPrettifyTests, prettifyBiasesFinalStaffWedgeTowardUpperLane)
{
    constexpr const char* targetHairpinEid = "WgiBdP/Vtz_g367TZXT7DJ";
    auto findTarget = [&](MasterScore* score) -> Hairpin*
    {
        for (const auto& pair : score->spanner())
        {
            Spanner* spanner = pair.second;
            if (spanner && spanner->isHairpin() && spanner->eid().toStdString() == targetHairpinEid)
            {
                return toHairpin(spanner);
            }
        }
        return nullptr;
    };

    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/compact-grandstaff-hairpin-lane.mscx");
    ASSERT_TRUE(score);
    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    relayoutScore(score);
    Hairpin* hairpin = findTarget(score);
    ASSERT_TRUE(hairpin);
    ASSERT_EQ(hairpin->spannerSegments().size(), 2);
    const Fraction startTick = hairpin->tick();
    const Fraction endTick = hairpin->tick2();
    const EID startEid = hairpin->startElement()->eid();
    const EID endEid = hairpin->endElement()->eid();

    std::vector<PointF> baselinePagePositions;
    std::vector<RectF> baselineShapes;
    for (SpannerSegment* segment : hairpin->spannerSegments())
    {
        segment->setOffset(segment->propertyDefault(Pid::OFFSET).value<PointF>());
        segment->setPropertyFlags(Pid::OFFSET, PropertyFlags::STYLED);
        segment->setAutoplace(true);
        segment->setPropertyFlags(Pid::AUTOPLACE, PropertyFlags::STYLED);
    }
    relayoutScore(score);
    for (SpannerSegment* segment : hairpin->spannerSegments())
    {
        baselinePagePositions.push_back(segment->pagePos());
        baselineShapes.push_back(segment->ldata()->shape().bbox());
    }

    const bool previousPrettify = MScore::pianomaniaPrettifySlursFingerings;
    MScore::pianomaniaPrettifySlursFingerings = true;
    relayoutScore(score);
    MScore::pianomaniaPrettifySlursFingerings = previousPrettify;

    ASSERT_EQ(hairpin->spannerSegments().size(), 2);
    for (size_t i = 0; i < hairpin->spannerSegments().size(); ++i)
    {
        SpannerSegment* segment = hairpin->segmentAt(static_cast<int>(i));
        ASSERT_TRUE(segment);
        EXPECT_TRUE(segment->placeAbove());
        EXPECT_EQ(segment->staff(), segment->part()->staves().back());
        EXPECT_TRUE(segment->autoplace());
        EXPECT_EQ(segment->ldata()->itemSnappedBefore(), nullptr);
        EXPECT_EQ(segment->ldata()->itemSnappedAfter(), nullptr);
        const double upwardMove = baselinePagePositions[i].y() - segment->pagePos().y();
        EXPECT_GT(upwardMove, 0.25 * segment->spatium());
        EXPECT_LT(upwardMove, 1.10 * segment->spatium());
        EXPECT_TRUE(pointNear(baselineShapes[i].topLeft(), segment->ldata()->shape().bbox().topLeft(), 0.01));
        EXPECT_TRUE(pointNear(baselineShapes[i].bottomRight(), segment->ldata()->shape().bbox().bottomRight(), 0.01));
    }
    EXPECT_EQ(hairpin->tick(), startTick);
    EXPECT_EQ(hairpin->tick2(), endTick);
    EXPECT_EQ(hairpin->startElement()->eid(), startEid);
    EXPECT_EQ(hairpin->endElement()->eid(), endEid);
    delete score;

    // Match ConsoleApp's three-flag export frame: both global flags are set
    // before score load, Auto Layout, the public Prettify command, and every
    // replay/save operation. The UI-style flags-only case above remains a
    // separate contract.
    struct CliPrettifyFlagsScope
    {
        bool previousPrettify = MScore::pianomaniaPrettifySlursFingerings;
        bool previousForceNormalize = MScore::pianomaniaForceNormalizeSlursFingerings;

        CliPrettifyFlagsScope()
        {
            MScore::pianomaniaPrettifySlursFingerings = true;
            MScore::pianomaniaForceNormalizeSlursFingerings = true;
        }

        ~CliPrettifyFlagsScope()
        {
            MScore::pianomaniaPrettifySlursFingerings = previousPrettify;
            MScore::pianomaniaForceNormalizeSlursFingerings = previousForceNormalize;
        }
    };

    auto physicalUpperSkylineGapSp = [](HairpinSegment* segment)
    {
        System* system = segment ? segment->system() : nullptr;
        const staff_idx_t thisIdx = segment ? segment->staffIdx() : muse::nidx;
        const staff_idx_t upperIdx = system ? system->prevVisibleStaff(thisIdx) : muse::nidx;
        SysStaff* thisStaff = system && thisIdx != muse::nidx ? system->staff(thisIdx) : nullptr;
        SysStaff* upperStaff = system && upperIdx != muse::nidx ? system->staff(upperIdx) : nullptr;
        if (!segment || !system || !thisStaff || !upperStaff)
        {
            return std::numeric_limits<double>::quiet_NaN();
        }

        const double elementXInSystem = segment->pageX() - system->pageX();
        const double horizontalClearance = system->style().styleMM(Sid::skylineMinHorizontalClearance);
        Shape shape = segment->ldata()
                          ->shape()
                          .translated(PointF(elementXInSystem, segment->y()))
                          .adjust(-horizontalClearance, 0.0, horizontalClearance, 0.0);
        shape.remove_if([](ShapeElement& shapeElement) { return shapeElement.ignoreForLayout(); });
        SkylineLine upperSkyline = upperStaff->skyline().south();
        upperSkyline.translateY(upperStaff->y() - thisStaff->y());
        return upperSkyline.verticalClaranceBelow(shape) / segment->spatium();
    };

    // Retain the UI-command property contract separately from the CLI frame.
    // In this path Prettify starts disabled, so the public command must create
    // and persist the bounded generated offset on both system segments.
    MasterScore* uiPersisted = ScoreRW::readScore(u"pianomania_prettify_data/compact-grandstaff-hairpin-lane.mscx");
    ASSERT_TRUE(uiPersisted);
    mu::engraving::pm::applyPianomaniaAutoLayout(uiPersisted);
    relayoutScore(uiPersisted);
    Hairpin* uiPersistedHairpin = findTarget(uiPersisted);
    ASSERT_TRUE(uiPersistedHairpin);
    EXPECT_TRUE(applyPrettifyCommand(uiPersisted).changed);
    ASSERT_EQ(uiPersistedHairpin->spannerSegments().size(), 2);
    for (SpannerSegment* segment : uiPersistedHairpin->spannerSegments())
    {
        const double persistedLift = segment->propertyDefault(Pid::OFFSET).value<PointF>().y() - segment->offset().y();
        EXPECT_GE(persistedLift, 0.25 * segment->spatium());
        EXPECT_LE(persistedLift, 0.40 * segment->spatium());
    }
    delete uiPersisted;

    {
        CliPrettifyFlagsScope cliPrettifyFlagsScope;
        MasterScore* persisted = ScoreRW::readScore(u"pianomania_prettify_data/compact-grandstaff-hairpin-lane.mscx");
        ASSERT_TRUE(persisted);
        mu::engraving::pm::applyPianomaniaAutoLayout(persisted);
        relayoutScore(persisted);
        Hairpin* persistedHairpin = findTarget(persisted);
        ASSERT_TRUE(persistedHairpin);
        // Auto Layout has already run with the same flags as ConsoleApp, so the
        // public command must be an idempotent no-op in this frame.
        EXPECT_FALSE(applyPrettifyCommand(persisted).changed);
        ASSERT_EQ(persistedHairpin->spannerSegments().size(), 2);
        const double physicalUpperGapSp = physicalUpperSkylineGapSp(toHairpinSegment(persistedHairpin->segmentAt(0)));
        ASSERT_TRUE(std::isfinite(physicalUpperGapSp));
        EXPECT_GE(physicalUpperGapSp, 0.25);
        EXPECT_LE(physicalUpperGapSp, 0.55);
        // The public force-normalization path must retain the upper-lane lift on
        // both systems. A flags-only layout pass does not prove export replay.
        for (size_t i = 0; i < persistedHairpin->spannerSegments().size(); ++i)
        {
            SpannerSegment* segment = persistedHairpin->segmentAt(static_cast<int>(i));
            RecordProperty(std::string("final_staff_wedge_segment_") + std::to_string(i),
                           std::to_string(physicalUpperSkylineGapSp(toHairpinSegment(segment))) + "," +
                               std::to_string(segment->pagePos().y() / segment->spatium()) + "," +
                               std::to_string(segment->offset().y() / segment->spatium()));
        }

        std::vector<PointF> offsets;
        std::vector<PropertyFlags> offsetFlags;
        std::vector<PointF> pagePositions;
        for (SpannerSegment* segment : persistedHairpin->spannerSegments())
        {
            offsets.push_back(segment->offset());
            offsetFlags.push_back(segment->propertyFlags(Pid::OFFSET));
            pagePositions.push_back(segment->pagePos());
        }
        relayoutScore(persisted);
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            SpannerSegment* segment = persistedHairpin->segmentAt(static_cast<int>(i));
            EXPECT_TRUE(pointNear(segment->offset(), offsets[i], 0.01));
            EXPECT_EQ(segment->propertyFlags(Pid::OFFSET), offsetFlags[i]);
            EXPECT_TRUE(pointNear(segment->pagePos(), pagePositions[i], 0.02 * segment->spatium()));
        }

        applyPrettifyCommand(persisted);
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            SpannerSegment* segment = persistedHairpin->segmentAt(static_cast<int>(i));
            EXPECT_TRUE(pointNear(segment->offset(), offsets[i], 0.01));
            EXPECT_EQ(segment->propertyFlags(Pid::OFFSET), offsetFlags[i]);
        }

        const std::string savedFileName = testing::TempDir() + "compact-grandstaff-hairpin-lane-roundtrip.mscz";
        const String savedPath = String::fromUtf8(savedFileName);
        muse::io::File savedFile(savedPath);
        ASSERT_TRUE(savedFile.open(muse::io::IODevice::WriteOnly));
        MscWriter::Params writerParams;
        writerParams.device = &savedFile;
        writerParams.filePath = savedPath;
        writerParams.mode = MscIoMode::Zip;
        MscWriter writer(writerParams);
        ASSERT_TRUE(writer.open());
        MscSaver saver(persisted->iocContext());
        ASSERT_TRUE(saver.writeMscz(persisted, writer, false));
        writer.close();
        ASSERT_FALSE(writer.hasError());
        savedFile.close();
        MasterScore* reloaded = ScoreRW::readScore(savedPath, true);
        ASSERT_TRUE(reloaded);
        relayoutScore(reloaded);
        Hairpin* reloadedHairpin = findTarget(reloaded);
        ASSERT_TRUE(reloadedHairpin);
        ASSERT_EQ(reloadedHairpin->spannerSegments().size(), offsets.size());
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            SpannerSegment* segment = reloadedHairpin->segmentAt(static_cast<int>(i));
            EXPECT_NEAR(segment->spatium(), persistedHairpin->spatium(), 0.01);
            EXPECT_NEAR(segment->offset().x() / segment->spatium(), offsets[i].x() / persistedHairpin->spatium(), 0.01);
            EXPECT_NEAR(segment->offset().y() / segment->spatium(), offsets[i].y() / persistedHairpin->spatium(), 0.01);
            EXPECT_EQ(segment->propertyFlags(Pid::OFFSET), offsetFlags[i]);
        }
        EXPECT_EQ(reloadedHairpin->tick(), startTick);
        EXPECT_EQ(reloadedHairpin->tick2(), endTick);
        EXPECT_EQ(reloadedHairpin->startElement()->eid(), startEid);
        EXPECT_EQ(reloadedHairpin->endElement()->eid(), endEid);

        applyPrettifyCommand(reloaded);
        std::vector<PointF> reloadedOffsets;
        std::vector<PointF> reloadedPositions;
        std::vector<RectF> reloadedShapes;
        for (size_t i = 0; i < offsets.size(); ++i)
        {
            SpannerSegment* segment = reloadedHairpin->segmentAt(static_cast<int>(i));
            reloadedOffsets.push_back(segment->offset());
            reloadedPositions.push_back(segment->pos());
            reloadedShapes.push_back(segment->ldata()->shape().bbox());
        }
        applyPrettifyCommand(reloaded);
        for (size_t i = 0; i < reloadedOffsets.size(); ++i)
        {
            SpannerSegment* segment = reloadedHairpin->segmentAt(static_cast<int>(i));
            EXPECT_TRUE(pointNear(segment->offset(), reloadedOffsets[i], 0.01 * segment->spatium()));
            EXPECT_NEAR(segment->pos().x() / segment->spatium(), reloadedPositions[i].x() / segment->spatium(), 0.01);
            EXPECT_NEAR(segment->pos().y() / segment->spatium(), reloadedPositions[i].y() / segment->spatium(), 0.01);
            EXPECT_TRUE(pointNear(segment->ldata()->shape().bbox().topLeft(), reloadedShapes[i].topLeft(), 0.01));
            EXPECT_TRUE(pointNear(segment->ldata()->shape().bbox().bottomRight(), reloadedShapes[i].bottomRight(), 0.01));
        }

        delete reloaded;
        delete persisted;
        std::remove(savedFileName.c_str());
    }

    enum class ControlKind
    {
        CenterOff,
        AutoplaceOff,
        ManualOffset
    };
    for (ControlKind kind : {ControlKind::CenterOff, ControlKind::AutoplaceOff, ControlKind::ManualOffset})
    {
        MasterScore* control = ScoreRW::readScore(u"pianomania_prettify_data/compact-grandstaff-hairpin-lane.mscx");
        ASSERT_TRUE(control);
        mu::engraving::pm::applyPianomaniaAutoLayout(control);
        relayoutScore(control);
        Hairpin* controlHairpin = findTarget(control);
        ASSERT_TRUE(controlHairpin);
        ASSERT_EQ(controlHairpin->spannerSegments().size(), 2);

        if (kind == ControlKind::CenterOff)
        {
            for (SpannerSegment* segment : controlHairpin->spannerSegments())
            {
                segment->setOffset(segment->propertyDefault(Pid::OFFSET).value<PointF>());
                segment->setPropertyFlags(Pid::OFFSET, PropertyFlags::STYLED);
            }
            controlHairpin->setProperty(Pid::CENTER_BETWEEN_STAVES, AutoOnOff::OFF);
        }
        else if (kind == ControlKind::AutoplaceOff)
        {
            for (SpannerSegment* segment : controlHairpin->spannerSegments())
            {
                segment->setOffset(segment->propertyDefault(Pid::OFFSET).value<PointF>());
                segment->setPropertyFlags(Pid::OFFSET, PropertyFlags::STYLED);
            }
            controlHairpin->setAutoplace(false);
        }
        else
        {
            for (SpannerSegment* segment : controlHairpin->spannerSegments())
            {
                segment->setOffset(segment->propertyDefault(Pid::OFFSET).value<PointF>());
                segment->setPropertyFlags(Pid::OFFSET, PropertyFlags::UNSTYLED);
            }
        }
        relayoutScore(control);

        std::vector<PointF> controlOffsets;
        std::vector<PropertyFlags> controlOffsetFlags;
        for (SpannerSegment* segment : controlHairpin->spannerSegments())
        {
            controlOffsets.push_back(segment->offset());
            controlOffsetFlags.push_back(segment->propertyFlags(Pid::OFFSET));
        }
        if (kind == ControlKind::ManualOffset)
        {
            ASSERT_TRUE(std::any_of(controlOffsetFlags.begin(), controlOffsetFlags.end(),
                                    [](PropertyFlags flags) { return flags != PropertyFlags::STYLED; }));
        }
        const bool previousControlPrettify = MScore::pianomaniaPrettifySlursFingerings;
        MScore::pianomaniaPrettifySlursFingerings = true;
        relayoutScore(control);
        MScore::pianomaniaPrettifySlursFingerings = previousControlPrettify;
        for (size_t i = 0; i < controlOffsets.size(); ++i)
        {
            SpannerSegment* segment = controlHairpin->segmentAt(static_cast<int>(i));
            EXPECT_TRUE(pointNear(segment->offset(), controlOffsets[i], 0.01));
            EXPECT_EQ(segment->propertyFlags(Pid::OFFSET), controlOffsetFlags[i]);
        }
        if (kind == ControlKind::CenterOff)
        {
            EXPECT_EQ(controlHairpin->centerBetweenStaves(), AutoOnOff::OFF);
        }
        else if (kind == ControlKind::AutoplaceOff)
        {
            EXPECT_FALSE(controlHairpin->autoplace());
        }
        delete control;
    }
}

namespace {

double graphicalHairpinLeft(const HairpinSegment* segment)
{
    const PointF pagePos = segment->pagePos();
    double result = pagePos.x() + segment->ldata()->points[0].x();
    for (size_t i = 1; i < 4; ++i) {
        result = std::min(result, pagePos.x() + segment->ldata()->points[i].x());
    }
    return result;
}

double graphicalHairpinRight(const HairpinSegment* segment)
{
    const PointF pagePos = segment->pagePos();
    double result = pagePos.x() + segment->ldata()->points[0].x();
    for (size_t i = 1; i < 4; ++i) {
        result = std::max(result, pagePos.x() + segment->ldata()->points[i].x());
    }
    return result;
}

std::vector<Hairpin*> hairpinsInTickOrder(Score* score)
{
    std::vector<Hairpin*> result;
    for (const auto& pair : score->spanner()) {
        if (pair.second && pair.second->isHairpin()) {
            result.push_back(toHairpin(pair.second));
        }
    }
    std::sort(result.begin(), result.end(), [](const Hairpin* a, const Hairpin* b) {
        return a->tick() < b->tick();
    });
    return result;
}

struct HairpinLineSnapshot
{
    PointF position;
    PointF endOffset;
    std::array<PointF, 4> points;
};

HairpinLineSnapshot captureHairpinLine(const HairpinSegment* segment)
{
    return HairpinLineSnapshot {
        segment->pos(), segment->pos2(),
        { segment->ldata()->points[0], segment->ldata()->points[1],
          segment->ldata()->points[2], segment->ldata()->points[3] }
    };
}

void expectHairpinLineEqual(const HairpinLineSnapshot& expected, const HairpinLineSnapshot& actual)
{
    EXPECT_EQ(actual.position, expected.position);
    EXPECT_EQ(actual.endOffset, expected.endOffset);
    EXPECT_EQ(actual.points, expected.points);
}

} // namespace

// Test value: Keeps the reported automatic Grieg crescendo-to-diminuendo
// mouth visibly separated under the coordinated export layout flags without
// changing its far endpoint, while manual, same-type, unsnapped, and authored
// offset pairs remain untouched.
TEST_F(Engraving_PianomaniaPrettifyTests, prettifySeparatesAutomaticFacingHairpinMouths)
{
    const String fixture = u"pianomania_prettify_data/snapped-facing-hairpins.mscx";
    auto targetPair = [](MasterScore* score) {
        std::vector<Hairpin*> hairpins = hairpinsInTickOrder(score);
        EXPECT_EQ(hairpins.size(), 2u);
        return hairpins;
    };
    auto segments = [](const std::vector<Hairpin*>& hairpins) {
        std::array<HairpinSegment*, 2> result { nullptr, nullptr };
        if (hairpins.size() == 2) {
            result[0] = toHairpinSegment(hairpins[0]->frontSegment());
            result[1] = toHairpinSegment(hairpins[1]->frontSegment());
        }
        return result;
    };
    auto gapSp = [](const std::array<HairpinSegment*, 2>& pair) {
        return (graphicalHairpinLeft(pair[1]) - graphicalHairpinRight(pair[0])) / pair[1]->spatium();
    };

    auto reportedGriegPair = [](MasterScore* score) {
        std::vector<Hairpin*> result;
        Hairpin* crescendo = nullptr;
        Hairpin* diminuendo = nullptr;
        for (Hairpin* hairpin : hairpinsInTickOrder(score)) {
            if (hairpin->hairpinType() == HairpinType::CRESC_HAIRPIN
                && hairpin->tick().ticks() == 8160 && hairpin->tick2().ticks() == 8280) {
                EXPECT_EQ(crescendo, nullptr);
                crescendo = hairpin;
            } else if (hairpin->hairpinType() == HairpinType::DIM_HAIRPIN
                       && hairpin->tick().ticks() == 8280) {
                EXPECT_EQ(diminuendo, nullptr);
                diminuendo = hairpin;
            }
        }
        if (crescendo && diminuendo) {
            result = { crescendo, diminuendo };
        }
        return result;
    };

    auto makeAutomaticPair = [](const std::vector<Hairpin*>& hairpins) {
        for (Hairpin* hairpin : hairpins) {
            ASSERT_TRUE(hairpin);
            hairpin->setAutoplace(true);
            static_cast<EngravingItem*>(hairpin)->setPropertyFlags(Pid::AUTOPLACE, PropertyFlags::STYLED);
            HairpinSegment* segment = toHairpinSegment(hairpin->frontSegment());
            ASSERT_TRUE(segment);
            segment->setAutoplace(true);
            static_cast<EngravingItem*>(segment)->setPropertyFlags(Pid::AUTOPLACE, PropertyFlags::STYLED);
            static_cast<EngravingItem*>(segment)->setPropertyFlags(Pid::OFFSET, PropertyFlags::STYLED);
            segment->setProperty(Pid::OFFSET2, PointF());
            static_cast<EngravingItem*>(segment)->setPropertyFlags(Pid::OFFSET2, PropertyFlags::NOSTYLE);
        }
    };

    // The reported pair was 0.3sp apart only because the source's dragged end
    // grips survived Auto Layout. Auto Layout now resets them, so recreate the
    // tight mouths with an automatic end offset to give Prettify a pair to
    // separate.
    constexpr double reportedGapSp = 0.30;
    auto tightenReportedPair = [&](MasterScore* score, std::array<HairpinSegment*, 2>& pair) {
        for (HairpinSegment* segment : pair) {
            EXPECT_TRUE(segment->getProperty(Pid::OFFSET2).value<PointF>().isNull());
        }
        const double naturalGapSp = gapSp(pair);
        EXPECT_GE(naturalGapSp, 0.60 - 0.001);
        pair[0]->setProperty(Pid::OFFSET2, PointF((naturalGapSp - reportedGapSp) * pair[0]->spatium(), 0.0));
        relayoutScore(score);
    };

    const String reportedFixture = u"pianomania_prettify_data/grieg-op12-no1-facing-hairpins.mscz";
    MasterScore* score = ScoreRW::readScore(reportedFixture);
    ASSERT_TRUE(score);
    const bool previousPrettify = MScore::pianomaniaPrettifySlursFingerings;
    const bool previousForceNormalize = MScore::pianomaniaForceNormalizeSlursFingerings;
    MScore::pianomaniaPrettifySlursFingerings = false;
    MScore::pianomaniaForceNormalizeSlursFingerings = true;
    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    relayoutScore(score);
    std::vector<Hairpin*> hairpins = reportedGriegPair(score);
    ASSERT_EQ(hairpins.size(), 2u);
    ASSERT_EQ(hairpins[0]->tick2(), hairpins[1]->tick());
    std::array<HairpinSegment*, 2> pair = segments(hairpins);
    ASSERT_TRUE(pair[0] && pair[1]);
    tightenReportedPair(score, pair);
    pair = segments(reportedGriegPair(score));
    EXPECT_NEAR(gapSp(pair), reportedGapSp, 0.001);
    const double ordinaryFarEndX = graphicalHairpinRight(pair[1]);
    delete score;

    score = ScoreRW::readScore(reportedFixture);
    ASSERT_TRUE(score);
    MScore::pianomaniaPrettifySlursFingerings = true;
    MScore::pianomaniaForceNormalizeSlursFingerings = true;
    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    relayoutScore(score);
    pair = segments(reportedGriegPair(score));
    ASSERT_TRUE(pair[0] && pair[1]);
    tightenReportedPair(score, pair);
    mu::engraving::pm::PmPrettifyOptions reportedOptions;
    reportedOptions.forceNormalizeManual = true;
    applyPrettifyCommand(score, reportedOptions);
    hairpins = reportedGriegPair(score);
    pair = segments(hairpins);
    ASSERT_TRUE(pair[0] && pair[1]);
    EXPECT_TRUE(hairpins[0]->snapToItemAfter());
    EXPECT_TRUE(hairpins[1]->snapToItemBefore());
    EXPECT_EQ(pair[0]->ldata()->itemSnappedAfter(), pair[1]);
    EXPECT_EQ(pair[1]->ldata()->itemSnappedBefore(), pair[0]);
    for (HairpinSegment* segment : pair) {
        EXPECT_TRUE(segment->visible());
        EXPECT_TRUE(segment->autoplace());
    }
    EXPECT_GE(gapSp(pair), 0.60 - 0.001);
    EXPECT_NEAR(graphicalHairpinRight(pair[1]), ordinaryFarEndX, 0.01);
    const HairpinLineSnapshot flagsOnCrescendo = captureHairpinLine(pair[0]);
    const HairpinLineSnapshot flagsOnDiminuendo = captureHairpinLine(pair[1]);
    applyPrettifyCommand(score, reportedOptions);
    pair = segments(reportedGriegPair(score));
    expectHairpinLineEqual(flagsOnCrescendo, captureHairpinLine(pair[0]));
    expectHairpinLineEqual(flagsOnDiminuendo, captureHairpinLine(pair[1]));
    EXPECT_GE(gapSp(pair), 0.60 - 0.001);
    EXPECT_NEAR(graphicalHairpinRight(pair[1]), ordinaryFarEndX, 0.01);
    MScore::pianomaniaPrettifySlursFingerings = previousPrettify;
    MScore::pianomaniaForceNormalizeSlursFingerings = previousForceNormalize;
    delete score;

    enum class ExcludedPair { Manual, SameType, CrescendoUnsnapped, DiminuendoUnsnapped };
    for (ExcludedPair control : { ExcludedPair::Manual, ExcludedPair::SameType,
                                  ExcludedPair::CrescendoUnsnapped, ExcludedPair::DiminuendoUnsnapped }) {
        MasterScore* excluded = ScoreRW::readScore(fixture);
        ASSERT_TRUE(excluded);
        mu::engraving::pm::applyPianomaniaAutoLayout(excluded);
        std::vector<Hairpin*> excludedHairpins = targetPair(excluded);
        makeAutomaticPair(excludedHairpins);
        if (control == ExcludedPair::Manual) {
            for (Hairpin* hairpin : excludedHairpins) {
                HairpinSegment* segment = toHairpinSegment(hairpin->frontSegment());
                ASSERT_TRUE(segment);
                segment->setAutoplace(false);
                static_cast<EngravingItem*>(segment)->setPropertyFlags(Pid::AUTOPLACE, PropertyFlags::UNSTYLED);
            }
        }
        ASSERT_EQ(excludedHairpins.size(), 2u);
        if (control == ExcludedPair::SameType) {
            excludedHairpins[1]->setHairpinType(HairpinType::CRESC_HAIRPIN);
        } else if (control == ExcludedPair::CrescendoUnsnapped) {
            excludedHairpins[0]->setSnapToItemAfter(false);
        } else if (control == ExcludedPair::DiminuendoUnsnapped) {
            excludedHairpins[1]->setSnapToItemBefore(false);
        }
        relayoutScore(excluded);
        std::array<HairpinSegment*, 2> excludedPair = segments(excludedHairpins);
        ASSERT_TRUE(excludedPair[0] && excludedPair[1]);
        if (control == ExcludedPair::Manual) {
            EXPECT_FALSE(excludedPair[0]->autoplace());
            EXPECT_FALSE(excludedPair[1]->autoplace());
        }
        const HairpinLineSnapshot beforeCrescendo = captureHairpinLine(excludedPair[0]);
        const HairpinLineSnapshot beforeSecond = captureHairpinLine(excludedPair[1]);
        const bool previousControlPrettify = MScore::pianomaniaPrettifySlursFingerings;
        MScore::pianomaniaPrettifySlursFingerings = true;
        relayoutScore(excluded);
        MScore::pianomaniaPrettifySlursFingerings = previousControlPrettify;
        expectHairpinLineEqual(beforeCrescendo, captureHairpinLine(excludedPair[0]));
        expectHairpinLineEqual(beforeSecond, captureHairpinLine(excludedPair[1]));
        delete excluded;
    }

    enum class PublicControl { AuthoredOffset, AuthoredOffset2, AuthoredZeroOffset2, ParentAutoplaceOff };
    for (PublicControl control : { PublicControl::AuthoredOffset, PublicControl::AuthoredOffset2,
                                   PublicControl::AuthoredZeroOffset2,
                                   PublicControl::ParentAutoplaceOff }) {
        MasterScore* excluded = ScoreRW::readScore(fixture);
        ASSERT_TRUE(excluded);
        mu::engraving::pm::applyPianomaniaAutoLayout(excluded);
        relayoutScore(excluded);
        std::vector<Hairpin*> excludedHairpins = targetPair(excluded);
        ASSERT_EQ(excludedHairpins.size(), 2u);
        makeAutomaticPair(excludedHairpins);
        std::array<HairpinSegment*, 2> excludedPair = segments(excludedHairpins);
        ASSERT_TRUE(excludedPair[0] && excludedPair[1]);
        if (control == PublicControl::AuthoredOffset) {
            excludedPair[1]->setOffset(excludedPair[1]->offset() + PointF(0.2 * excludedPair[1]->spatium(), 0.0));
            static_cast<EngravingItem*>(excludedPair[1])->setPropertyFlags(Pid::OFFSET, PropertyFlags::UNSTYLED);
        } else if (control == PublicControl::AuthoredOffset2) {
            const PointF offset2 = excludedPair[1]->getProperty(Pid::OFFSET2).value<PointF>();
            excludedPair[1]->setProperty(Pid::OFFSET2, offset2 + PointF(-0.2 * excludedPair[1]->spatium(), 0.0));
            static_cast<EngravingItem*>(excludedPair[1])->setPropertyFlags(Pid::OFFSET2, PropertyFlags::UNSTYLED);
        } else if (control == PublicControl::AuthoredZeroOffset2) {
            excludedPair[1]->setProperty(Pid::OFFSET2, PointF());
            static_cast<EngravingItem*>(excludedPair[1])->setPropertyFlags(Pid::OFFSET2, PropertyFlags::UNSTYLED);
        } else {
            excludedHairpins[0]->setAutoplace(false);
            excludedHairpins[1]->setAutoplace(false);
        }
        relayoutScore(excluded);
        excludedPair = segments(excludedHairpins);
        const HairpinLineSnapshot beforeCrescendo = captureHairpinLine(excludedPair[0]);
        const HairpinLineSnapshot beforeDiminuendo = captureHairpinLine(excludedPair[1]);
        const PointF beforeOffset = excludedPair[1]->offset();
        const PropertyFlags beforeOffsetFlags = excludedPair[1]->propertyFlags(Pid::OFFSET);
        const PointF beforeOffset2 = excludedPair[1]->getProperty(Pid::OFFSET2).value<PointF>();
        const PropertyFlags beforeOffset2Flags = excludedPair[1]->propertyFlags(Pid::OFFSET2);
        mu::engraving::pm::PmPrettifyOptions options;
        options.forceNormalizeManual = false;
        applyPrettifyCommand(excluded, options);
        excludedPair = segments(targetPair(excluded));
        expectHairpinLineEqual(beforeCrescendo, captureHairpinLine(excludedPair[0]));
        expectHairpinLineEqual(beforeDiminuendo, captureHairpinLine(excludedPair[1]));
        EXPECT_EQ(excludedPair[1]->offset(), beforeOffset);
        EXPECT_EQ(excludedPair[1]->propertyFlags(Pid::OFFSET), beforeOffsetFlags);
        EXPECT_EQ(excludedPair[1]->getProperty(Pid::OFFSET2).value<PointF>(), beforeOffset2);
        EXPECT_EQ(excludedPair[1]->propertyFlags(Pid::OFFSET2), beforeOffset2Flags);
        if (control == PublicControl::ParentAutoplaceOff) {
            EXPECT_FALSE(excludedHairpins[0]->autoplace());
            EXPECT_FALSE(excludedHairpins[1]->autoplace());
        }
        delete excluded;
    }
    MScore::pianomaniaPrettifySlursFingerings = previousPrettify;
}

namespace {
// The flags the Practice exporter lays out with.
struct PracticeExportFlags {
    bool previousPrettify = MScore::pianomaniaPrettifySlursFingerings;
    bool previousForceNormalize = MScore::pianomaniaForceNormalizeSlursFingerings;

    PracticeExportFlags()
    {
        MScore::pianomaniaPrettifySlursFingerings = true;
        MScore::pianomaniaForceNormalizeSlursFingerings = true;
    }

    ~PracticeExportFlags()
    {
        MScore::pianomaniaPrettifySlursFingerings = previousPrettify;
        MScore::pianomaniaForceNormalizeSlursFingerings = previousForceNormalize;
    }
};

MasterScore* readAutoLaidOut(const String& fixture)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/" + fixture);
    if (score) {
        mu::engraving::pm::applyPianomaniaAutoLayout(score);
        relayoutScore(score);
    }
    return score;
}

std::vector<Chord*> collectChords(Score* score)
{
    std::vector<Chord*> chords;
    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(SegmentType::ChordRest); segment; segment = segment->next(SegmentType::ChordRest)) {
            for (EngravingItem* item : segment->elist()) {
                if (item && item->isChord()) {
                    chords.push_back(toChord(item));
                }
            }
        }
    }
    return chords;
}

std::vector<RectF> crossStaffBeamPageRects(Score* score)
{
    std::set<const Beam*> beams;
    for (Chord* chord : collectChords(score)) {
        if (chord->beam() && chord->beam()->cross()) {
            beams.insert(chord->beam());
        }
    }
    std::vector<RectF> rects;
    for (const Beam* beam : beams) {
        for (const RectF& rect : beam->shape().translated(beam->pagePos()).toRects()) {
            rects.push_back(rect);
        }
    }
    return rects;
}

// Noteheads and accidentals of one staff (cross-staff notes count where they are drawn).
std::vector<RectF> notePageRects(Score* score, staff_idx_t vStaffIdx)
{
    std::vector<RectF> rects;
    for (Chord* chord : collectChords(score)) {
        if (chord->vStaffIdx() != vStaffIdx) {
            continue;
        }
        for (Note* note : chord->notes()) {
            rects.push_back(note->pageBoundingRect());
            if (note->accidental() && note->accidental()->visible()) {
                rects.push_back(note->accidental()->pageBoundingRect());
            }
        }
    }
    return rects;
}

double staffLinePageY(const System* system, staff_idx_t staffIdx, int line, const Fraction& tick)
{
    const Staff* staff = system->score()->staff(staffIdx);
    return system->pagePos().y() + system->staff(staffIdx)->y() + line * staff->lineDistance(tick) * staff->spatium(tick);
}

std::vector<PointF> hairpinPagePoints(const HairpinSegment* segment)
{
    std::vector<PointF> points;
    for (size_t i = 0; i < 4; ++i) {
        points.push_back(segment->pagePos() + segment->ldata()->points[i]);
    }
    return points;
}
}

// Test value: Auto Layout returns a hairpin end grip dragged before the last
// note of its span, and a dragged ottava hook, to their automatic placement.
TEST_F(Engraving_PianomaniaPrettifyTests, autoLayoutResetsDraggedLineEndsAndOttavaHooks)
{
    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/line-end-and-ottava-hook-normalization.mscx");
    ASSERT_TRUE(score);
    std::vector<Hairpin*> hairpins = collectHairpins(score);
    ASSERT_EQ(hairpins.size(), 1u);
    Hairpin* hairpin = hairpins.front();
    Ottava* ottava = nullptr;
    for (const auto& pair : score->spanner()) {
        if (pair.second && pair.second->isOttava()) {
            ottava = toOttava(pair.second);
        }
    }
    ASSERT_TRUE(ottava);
    ASSERT_FALSE(hairpin->spannerSegments().empty());
    EXPECT_FALSE(hairpin->frontSegment()->getProperty(Pid::OFFSET2).value<PointF>().isNull());
    EXPECT_EQ(ottava->propertyFlags(Pid::END_HOOK_HEIGHT), PropertyFlags::UNSTYLED);

    mu::engraving::pm::applyPianomaniaAutoLayout(score);
    relayoutScore(score);

    HairpinSegment* segment = toHairpinSegment(hairpin->frontSegment());
    ASSERT_TRUE(segment);
    EXPECT_TRUE(segment->getProperty(Pid::OFFSET2).value<PointF>().isNull());
    const Measure* first = score->firstMeasure();
    double lastNoteRight = -std::numeric_limits<double>::max();
    for (Chord* chord : collectChords(score)) {
        if (chord->measure() == first && chord->staffIdx() == 0) {
            for (Note* note : chord->notes()) {
                lastNoteRight = std::max(lastNoteRight, note->pageBoundingRect().right());
            }
        }
    }
    const Segment* endBarline = first->findSegment(SegmentType::EndBarLine, first->endTick());
    ASSERT_TRUE(endBarline);
    EXPECT_GT(graphicalHairpinRight(segment), lastNoteRight);
    EXPECT_LT(graphicalHairpinRight(segment), endBarline->pageX());

    EXPECT_TRUE(ottava->isStyled(Pid::END_HOOK_HEIGHT));
    EXPECT_EQ(ottava->getProperty(Pid::END_HOOK_HEIGHT), ottava->propertyDefault(Pid::END_HOOK_HEIGHT));
    EXPECT_EQ(ottava->getProperty(Pid::BEGIN_HOOK_HEIGHT), ottava->propertyDefault(Pid::BEGIN_HOOK_HEIGHT));

    delete score;
}

// Test value: A staccato on the stem side of a beamed note centres on the
// notehead (Gurlitt Op. 101 No. 8), not halfway towards the stem.
TEST_F(Engraving_PianomaniaPrettifyTests, stemSideStaccatosCentreOnTheirNoteheads)
{
    MasterScore* score = readAutoLaidOut(u"stem-side-staccatos-under-beams.mscx");
    ASSERT_TRUE(score);

    auto stemSideStaccatoOffsets = [](Score* current) {
        std::vector<double> offsets;
        for (Chord* chord : collectChords(current)) {
            for (Articulation* articulation : chord->articulations()) {
                if (!articulation->isStaccato() || articulation->up() != chord->up() || !chord->stem()) {
                    continue;
                }
                const Note* head = chord->up() ? chord->downNote() : chord->upNote();
                offsets.push_back((articulation->pageBoundingRect().center().x() - head->pageBoundingRect().center().x())
                                  / articulation->spatium());
            }
        }
        return offsets;
    };

    const std::vector<double> centred = stemSideStaccatoOffsets(score);
    ASSERT_GE(centred.size(), 2u);
    for (double offset : centred) {
        EXPECT_NEAR(offset, 0.0, 0.02);
    }

    // The fixture exercises the rule: halfway alignment moves every mark.
    score->style().set(Sid::articulationStemHAlign, int(ArticulationStemSideAlign::AVERAGE));
    relayoutScore(score);
    for (double offset : stemSideStaccatoOffsets(score)) {
        EXPECT_GT(std::abs(offset), 0.1);
    }

    delete score;
}

// Test value: A tuplet number on the beam side never straddles a staff line
// (Chopin Op. 9 No. 1 m3 "11"); the source's tuplets-inside-the-staff style
// put it across the bottom line.
TEST_F(Engraving_PianomaniaPrettifyTests, beamSideTupletNumberStaysOffStaffLines)
{
    MasterScore* score = readAutoLaidOut(u"beam-side-tuplet-number.mscx");
    ASSERT_TRUE(score);

    auto straddlingNumbers = [](Score* current) {
        size_t straddling = 0;
        size_t numbers = 0;
        for (Tuplet* tuplet : collectTuplets(current)) {
            const Text* number = tuplet->number();
            if (!number || !number->visible() || number->ldata()->isSkipDraw() || tuplet->cross()) {
                continue;
            }
            const System* system = tuplet->measure()->system();
            const RectF bounds = number->pageBoundingRect();
            ++numbers;
            const int lines = current->staff(tuplet->staffIdx())->lines(tuplet->tick());
            for (int line = 0; line < lines; ++line) {
                const double y = staffLinePageY(system, tuplet->staffIdx(), line, tuplet->tick());
                if (bounds.top() < y && bounds.bottom() > y) {
                    ++straddling;
                    break;
                }
            }
        }
        return std::make_pair(numbers, straddling);
    };

    const auto [numbers, straddling] = straddlingNumbers(score);
    ASSERT_GE(numbers, 1u);
    EXPECT_EQ(straddling, 0u);

    // The fixture exercises the rule: tuplets inside the staff cross a line.
    score->style().set(Sid::tupletOutOfStaff, false);
    relayoutScore(score);
    EXPECT_GE(straddlingNumbers(score).second, 1u);

    delete score;
}

// Test value: A centred dynamic in the grand-staff gap clears a cross-staff
// beam rising from the lower staff (Beethoven Op. 27 No. 2 m5 "pp").
TEST_F(Engraving_PianomaniaPrettifyTests, staffCenteredDynamicClearsCrossStaffBeam)
{
    PracticeExportFlags flags;
    MasterScore* score = readAutoLaidOut(u"staff-centered-dynamic-cross-staff-beam.mscx");
    ASSERT_TRUE(score);

    const std::vector<Dynamic*> dynamics = collectAnnotations<Dynamic>(score, &EngravingObject::isDynamic);
    ASSERT_EQ(dynamics.size(), 1u);
    const Dynamic* dynamic = dynamics.front();
    const RectF bounds = dynamic->pageBoundingRect();
    const std::vector<RectF> beams = crossStaffBeamPageRects(score);
    ASSERT_FALSE(beams.empty());
    for (const RectF& beam : beams) {
        EXPECT_FALSE(rectsOverlap(bounds, beam));
    }
    for (staff_idx_t staffIdx : { staff_idx_t(0), staff_idx_t(1) }) {
        for (const RectF& head : notePageRects(score, staffIdx)) {
            EXPECT_FALSE(rectsOverlap(bounds, head));
        }
    }
    const System* system = dynamic->segment()->measure()->system();
    EXPECT_GT(bounds.top(), staffLinePageY(system, 0, 4, dynamic->tick()));
    EXPECT_LT(bounds.bottom(), staffLinePageY(system, 1, 0, dynamic->tick()));

    delete score;
}

// Test value: Centred hairpins under cross-staff beams sit below the beams and
// cross only the lower staff's stems (C. P. E. Bach H. 220 m7-8).
TEST_F(Engraving_PianomaniaPrettifyTests, staffCenteredHairpinsSitBelowCrossStaffBeams)
{
    PracticeExportFlags flags;
    MasterScore* score = readAutoLaidOut(u"staff-centered-hairpins-cross-staff-beams.mscx");
    ASSERT_TRUE(score);

    const std::vector<RectF> beams = crossStaffBeamPageRects(score);
    ASSERT_FALSE(beams.empty());
    size_t checked = 0;
    for (Hairpin* hairpin : collectHairpins(score)) {
        for (SpannerSegment* spannerSegment : hairpin->spannerSegments()) {
            const HairpinSegment* segment = toHairpinSegment(spannerSegment);
            const std::vector<PointF> points = hairpinPagePoints(segment);
            double left = std::numeric_limits<double>::max();
            double right = -left;
            double top = std::numeric_limits<double>::max();
            double bottom = -top;
            for (const PointF& point : points) {
                left = std::min(left, point.x());
                right = std::max(right, point.x());
                top = std::min(top, point.y());
                bottom = std::max(bottom, point.y());
            }
            for (const RectF& beam : beams) {
                if (beam.right() <= left || beam.left() >= right) {
                    continue;
                }
                EXPECT_GT(top, beam.bottom());
            }
            EXPECT_LT(bottom, staffLinePageY(segment->system(), 1, 0, hairpin->tick()));
            ++checked;
        }
    }
    EXPECT_EQ(checked, 2u);

    delete score;
}

// Test value: A long slur passing over a chord clears the accent that a shorter
// slur ending on that chord pushed outside itself (Chopin Op. 9 No. 1 m30).
TEST_F(Engraving_PianomaniaPrettifyTests, slurClearsMarkMovedOutsideShorterSlur)
{
    PracticeExportFlags flags;
    MasterScore* score = readAutoLaidOut(u"slur-over-mark-outside-short-slur.mscx");
    ASSERT_TRUE(score);

    Slur* longSlur = nullptr;
    for (const auto& pair : score->spanner()) {
        Slur* slur = pair.second && pair.second->isSlur() ? toSlur(pair.second) : nullptr;
        if (slur && slur->staffIdx() == 0 && (!longSlur || slur->ticks() > longSlur->ticks())) {
            longSlur = slur;
        }
    }
    ASSERT_TRUE(longSlur);
    ASSERT_EQ(longSlur->nsegments(), 1u);
    SlurSegment* slurSegment = longSlur->frontSegment();

    size_t checked = 0;
    for (Chord* chord : collectChords(score)) {
        if (chord->tick() <= longSlur->tick() || chord->tick() >= longSlur->tick2() || chord->staffIdx() != 0) {
            continue;
        }
        for (Articulation* accent : chord->articulations()) {
            if (!accent->isAccent() || accent->up() != longSlur->up()) {
                continue;
            }
            const double spatium = accent->spatium();
            Shape accentShape(Shape::Type::Composite);
            accentShape.add(accent->shape().translated(accent->pagePos()));
            accentShape.add(accent->ldata()->bbox().translated(accent->pagePos()));
            Shape slurShape = slurSegment->shape().translated(slurSegment->pagePos());
            slurShape.add(sampledPathShape(slurSegment->ldata()->path(), slurSegment->pagePos()));
            const double clearance = longSlur->up() ? slurShape.verticalClearance(accentShape, 0.0)
                                     : accentShape.verticalClearance(slurShape, 0.0);
            EXPECT_GE(clearance, 0.1 * spatium);
            ++checked;
        }
    }
    EXPECT_GE(checked, 1u);

    delete score;
}

namespace {
// The single digit on the note of the given pitch in the given (1-based) measure.
Fingering* fingeringOnPitch(Score* score, int measureNumber, int pitch, const String& text)
{
    Fingering* found = nullptr;
    for (Fingering* fingering : collectFingeringsByText(score, text)) {
        const Note* note = fingering->note();
        if (note && note->pitch() == pitch && note->chord()->measure()->no() + 1 == measureNumber) {
            EXPECT_EQ(found, nullptr) << "ambiguous digit " << text.toStdString() << " in m" << measureNumber;
            found = fingering;
        }
    }
    return found;
}
}

// Test value: Tchaikovsky Op. 39 No. 1 m17/m19 (and Goedicke m21). Two
// right-hand voices striking a tritone dyad (F#/C) together are one grip:
// their digits stack in one column above the staff, the larger number
// higher ("4 over 1", "4 over 2"), instead of the lower voice's digit
// dropping below the staff where it reads as the left hand's.
TEST_F(Engraving_PianomaniaPrettifyTests, sameHandTritoneDyadStacksAboveInOneColumn)
{
    PracticeExportFlags flags;
    MasterScore* score = readAutoLaidOut(u"fingering-same-hand-tritone-dyad.mscx");
    ASSERT_TRUE(score);

    struct Dyad {
        int measure;
        int upperPitch;
        int lowerPitch;
        String lowerDigit;
    };
    for (const Dyad& dyad : { Dyad { 1, 78, 72, u"1" }, Dyad { 2, 66, 60, u"2" } }) {
        const Fingering* upper = fingeringOnPitch(score, dyad.measure, dyad.upperPitch, u"4");
        ASSERT_TRUE(upper) << "m" << dyad.measure;
        // The lower voice repeats its pitch later in the measure; the dyad's
        // digit is the one on the attack shared with the upper voice.
        const Fingering* lower = nullptr;
        for (Fingering* fingering : collectFingeringsByText(score, dyad.lowerDigit)) {
            if (fingering->note()->pitch() == dyad.lowerPitch
                && fingering->note()->chord()->tick() == upper->note()->chord()->tick()) {
                lower = fingering;
            }
        }
        ASSERT_TRUE(lower) << "m" << dyad.measure;
        ASSERT_NE(lower->note()->chord(), upper->note()->chord()) << "the dyad spans two voices";

        EXPECT_EQ(upper->placement(), PlacementV::ABOVE) << "m" << dyad.measure;
        EXPECT_EQ(lower->placement(), PlacementV::ABOVE) << "m" << dyad.measure;
        const double sp = upper->spatium();
        const RectF upperRect = fingeringSystemRect(upper);
        const RectF lowerRect = fingeringSystemRect(lower);
        // One column: centred together, "4" on top, both above the upper notehead.
        EXPECT_NEAR(upperRect.center().x(), lowerRect.center().x(), 0.1 * sp) << "m" << dyad.measure;
        EXPECT_LE(upperRect.bottom(), lowerRect.top() + 0.01 * sp) << "m" << dyad.measure;
        EXPECT_LT(lowerRect.bottom(), noteSystemRect(upper->note()).top()) << "m" << dyad.measure;
    }

    delete score;
}

// Test value: Moonlight m1/m3. Hidden tuplets print nothing, so they cannot
// push a digit off its side: the triplet's middle digit stays above with its
// neighbours instead of dropping below the notes.
TEST_F(Engraving_PianomaniaPrettifyTests, hiddenTupletLeavesDigitsOnHandSide)
{
    PracticeExportFlags flags;
    MasterScore* score = readAutoLaidOut(u"fingering-hidden-tuplet.mscx");
    ASSERT_TRUE(score);

    for (Tuplet* tuplet : collectTuplets(score)) {
        ASSERT_FALSE(tuplet->visible());
    }
    size_t checked = 0;
    for (const String& text : { String(u"1"), String(u"2"), String(u"3"), String(u"4"), String(u"5") }) {
        for (const Fingering* fingering : collectFingeringsByText(score, text)) {
            EXPECT_EQ(fingering->placement(), PlacementV::ABOVE) << text.toStdString();
            EXPECT_LT(fingeringSystemRect(fingering).bottom(), noteSystemRect(fingering->note()).top()) << text.toStdString();
            ++checked;
        }
    }
    EXPECT_EQ(checked, 5u);

    delete score;
}

// Test value: Promenade m1/m3. A left-hand triplet beamed below its notes
// would put its "3" where the left-hand digits go, which used to throw the
// middle digit above the notes. The digits keep their side and the number
// takes the other side of the group; Prettify persists that side.
TEST_F(Engraving_PianomaniaPrettifyTests, tupletNumberYieldsSideToLeftHandDigits)
{
    auto assertDigitsBelowAndNumberAbove = [](Score* score) {
        const std::vector<Tuplet*> tuplets = collectTuplets(score);
        ASSERT_EQ(tuplets.size(), 1u);
        const Tuplet* tuplet = tuplets.front();
        EXPECT_TRUE(tuplet->isUp());
        const RectF numberRect = tupletNumberSystemRect(tuplet);
        ASSERT_FALSE(numberRect.isNull());

        double noteheadsTop = std::numeric_limits<double>::max();
        for (const DurationElement* element : tuplet->elements()) {
            ASSERT_TRUE(element->isChord());
            const Chord* chord = toChord(element);
            EXPECT_FALSE(chord->up()) << "the beam stays below the triplet";
            for (const Note* note : chord->notes()) {
                noteheadsTop = std::min(noteheadsTop, note->pageBoundingRect().top());
            }
        }
        EXPECT_LT(numberRect.bottom(), noteheadsTop);

        size_t checked = 0;
        for (const String& text : { String(u"1"), String(u"2"), String(u"3") }) {
            for (const Fingering* fingering : collectFingeringsByText(score, text)) {
                if (fingering->note()->chord()->measure()->no() != 0) {
                    continue;
                }
                EXPECT_EQ(fingering->placement(), PlacementV::BELOW) << text.toStdString();
                EXPECT_GT(fingeringSystemRect(fingering).top(), noteSystemRect(fingering->note()).bottom()) << text.toStdString();
                EXPECT_FALSE(rectsOverlap(fingering->pageBoundingRect(), numberRect)) << text.toStdString();
                ++checked;
            }
        }
        EXPECT_EQ(checked, 5u);
    };

    {
        PracticeExportFlags flags;
        MasterScore* score = readAutoLaidOut(u"fingering-tuplet-number-yields.mscx");
        ASSERT_TRUE(score);
        EXPECT_EQ(collectTuplets(score).front()->direction(), DirectionV::AUTO);
        assertDigitsBelowAndNumberAbove(score);
        delete score;
    }

    MasterScore* score = ScoreRW::readScore(u"pianomania_prettify_data/fingering-tuplet-number-yields.mscx");
    ASSERT_TRUE(score);
    relayoutScore(score);
    EXPECT_TRUE(applyPrettifyCommand(score).changed);
    EXPECT_EQ(collectTuplets(score).front()->direction(), DirectionV::UP);
    assertDigitsBelowAndNumberAbove(score);
    applyPrettifyCommand(score);
    EXPECT_EQ(collectTuplets(score).front()->direction(), DirectionV::UP);
    assertDigitsBelowAndNumberAbove(score);
    delete score;
}

// Test value: Bartok For Children I/3 m8/m18. A digit over a marcato on the
// same note stacks centred on the wedge with a real gap, instead of the
// near-touch squeeze that suits staccato dots.
TEST_F(Engraving_PianomaniaPrettifyTests, digitOverMarcatoKeepsClearGap)
{
    PracticeExportFlags flags;
    MasterScore* score = readAutoLaidOut(u"fingering-marcato-stack.mscx");
    ASSERT_TRUE(score);

    size_t checked = 0;
    for (const Fingering* fingering : collectFingeringsByText(score, u"1")) {
        const Chord* chord = fingering->note()->chord();
        for (const Articulation* articulation : chord->articulations()) {
            if (!articulation->isMarcato()) {
                continue;
            }
            const double sp = fingering->spatium();
            const RectF digitRect = fingering->pageBoundingRect();
            const RectF marcatoRect = articulation->pageBoundingRect();
            EXPECT_EQ(fingering->placement(), PlacementV::ABOVE);
            EXPECT_LE(digitRect.bottom(), marcatoRect.top() - 0.25 * sp + 1e-3);
            EXPECT_NEAR(digitRect.center().x(), marcatoRect.center().x(), 0.1 * sp);
            ++checked;
        }
    }
    EXPECT_EQ(checked, 1u);

    delete score;
}

// Test value: Clementi Op. 36 No. 2 I m36. The "1" on the last sixteenth of a
// beamed group fits under the phrase slur its neighbours tuck under: resting
// on its beam at the staff-line floor, under the slur, at most half a space
// toward its stem — not lifted over the slur.
TEST_F(Engraving_PianomaniaPrettifyTests, digitTucksUnderPhraseSlurOnItsBeam)
{
    PracticeExportFlags flags;
    MasterScore* score = readAutoLaidOut(u"clementi-op36-no2-i-under-slur-pocket.mscz");
    ASSERT_TRUE(score);

    const Fingering* digit = fingeringOnPitch(score, 36, 67, u"1");
    ASSERT_TRUE(digit);
    const Chord* chord = digit->note()->chord();
    ASSERT_TRUE(chord->up());
    ASSERT_TRUE(chord->beam());
    EXPECT_EQ(digit->placement(), PlacementV::ABOVE);
    const double sp = digit->spatium();
    const RectF digitRect = digit->pageBoundingRect();
    const RectF noteRect = digit->note()->pageBoundingRect();
    EXPECT_GE(digitRect.center().x(), noteRect.center().x() - 0.01 * sp);
    EXPECT_LE(digitRect.center().x(), noteRect.center().x() + 0.5 * sp + 0.01 * sp);

    // On top of the beam, with the staff-line floor.
    const Beam* beam = chord->beam();
    const PointF beamOrigin = beam->pagePos();
    double beamTop = std::numeric_limits<double>::max();
    for (const BeamSegment* beamSegment : beam->beamSegments()) {
        const PointF start = beamSegment->line.p1() + beamOrigin;
        const PointF end = beamSegment->line.p2() + beamOrigin;
        for (double x : { digitRect.left(), digitRect.right() }) {
            const double clamped = std::clamp(x, std::min(start.x(), end.x()), std::max(start.x(), end.x()));
            const double centerY = start.y() + (end.y() - start.y()) * (clamped - start.x()) / (end.x() - start.x());
            beamTop = std::min(beamTop, centerY - 0.5 * beam->beamWidth());
        }
    }
    EXPECT_LE(digitRect.bottom(), beamTop - 0.1 * sp + 1e-3);

    // Under the covering slur: every curve point over the digit is above it.
    const Slur* covering = nullptr;
    for (const auto& pair : score->spanner()) {
        const Slur* slur = pair.second && pair.second->isSlur() ? toSlur(pair.second) : nullptr;
        if (slur && slur->tick() < chord->tick() && slur->tick2() > chord->tick() && slur->track() == chord->track()) {
            covering = slur;
        }
    }
    ASSERT_TRUE(covering);
    size_t samples = 0;
    for (const SpannerSegment* spannerSegment : covering->spannerSegments()) {
        const SlurSegment* segment = toSlurSegment(spannerSegment);
        const PointF origin = segment->pagePos();
        const CubicBezier bezier(origin + segment->ups(Grip::START).pos(), origin + segment->ups(Grip::BEZIER1).pos(),
                                 origin + segment->ups(Grip::BEZIER2).pos(), origin + segment->ups(Grip::END).pos());
        for (int i = 0; i <= 200; ++i) {
            const PointF point = bezier.pointAtPercent(i / 200.0);
            if (point.x() < digitRect.left() || point.x() > digitRect.right()) {
                continue;
            }
            EXPECT_LT(point.y() + 0.5 * segment->ldata()->midThickness(), digitRect.top());
            ++samples;
        }
    }
    EXPECT_GT(samples, 0u);

    delete score;
}
