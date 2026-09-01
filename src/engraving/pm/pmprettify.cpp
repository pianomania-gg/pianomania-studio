/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */

#include "pmprettify.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>
#include <vector>

#include "log.h"

#include "../dom/engravingitem.h"
#include "../dom/fingering.h"
#include "../dom/hairpin.h"
#include "../dom/measure.h"
#include "../dom/mscore.h"
#include "../dom/page.h"
#include "../dom/score.h"
#include "../dom/slur.h"
#include "../dom/spanner.h"
#include "../dom/system.h"

using namespace mu::engraving;
using namespace mu::engraving::pm;

namespace {

struct PianomaniaSlurSegmentTarget
{
    Slur* slur = nullptr;
    size_t segmentIndex = 0;
    std::array<PointF, 4> gripPositions;
};

struct PianomaniaFingeringTarget
{
    Fingering* fingering = nullptr;
    PointF pos;
    PlacementV placement = PlacementV::ABOVE;
    // Offset/min-distance state at the end of the flags-on layout. The
    // prettify pass reaches its final digit positions partly through
    // Autoplace rebases (offset + released MIN_DISTANCE floor) that make every
    // *subsequent* layout place the digit correctly before skylines are built.
    // The position-delta loop cannot recreate those rebases — the pass is
    // deterministic, so the measured deltas are zero — and without them the
    // replay's pre-pass layout leaves stale lofted digit rects in the
    // skylines, inflating layout2 staff distances the transient layout never
    // saw. Mirroring the flags-on properties reproduces the converged state.
    PointF flagsOnOffset;
    PropertyFlags flagsOnOffsetFlags = PropertyFlags::STYLED;
    double flagsOnMinDistance = 0.0;
    PropertyFlags flagsOnMinDistanceFlags = PropertyFlags::STYLED;
    // Auto (non-manual) in the neutral state, so the prettify pass owns the
    // digit and the replay may pin it; manual digits keep their own state.
    bool pmControlled = false;
};

// The transient hairpin-over-slur lift (liftPianomaniaHairpinChainsClearOfSlurs)
// only exists while the prettify layout flag is on; the replay layout needs the
// lift persisted as a segment offset. Only y is persisted: x is anchor-driven.
struct PianomaniaHairpinSegmentTarget
{
    Hairpin* hairpin = nullptr;
    size_t segmentIndex = 0;
    double posY = 0.0;
};

struct PianomaniaPrettifyTargets
{
    std::vector<PianomaniaSlurSegmentTarget> slurSegments;
    std::vector<PianomaniaFingeringTarget> fingerings;
    std::vector<PianomaniaHairpinSegmentTarget> hairpinSegments;
};

struct PianomaniaSlurSnapshotEntry
{
    Slur* slur = nullptr;
    size_t segmentIndex = 0;
    std::array<PointF, 4> offsets;
    std::array<PropertyFlags, 4> offsetFlags;
    PointF offset;
    PropertyFlags generalOffsetFlags = PropertyFlags::STYLED;
    bool autoplace = true;
    PropertyFlags autoplaceFlags = PropertyFlags::STYLED;
    PointF endPointOff1;
    PointF endPointOff2;
    double extraHeight = 0.0;
    OffsetChange offsetChanged = OffsetChange::NONE;
    PointF changedPos;
    double spatium = 1.0;
};

struct PianomaniaFingeringSnapshotEntry
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

struct PianomaniaHairpinSnapshotEntry
{
    Hairpin* hairpin = nullptr;
    size_t segmentIndex = 0;
    PointF offset;
    PropertyFlags offsetFlags = PropertyFlags::STYLED;
    OffsetChange offsetChanged = OffsetChange::NONE;
    PointF changedPos;
    double spatium = 1.0;
};

struct PianomaniaPrettifySnapshot
{
    std::vector<PianomaniaSlurSnapshotEntry> slurSegments;
    std::vector<PianomaniaFingeringSnapshotEntry> fingerings;
    std::vector<PianomaniaHairpinSnapshotEntry> hairpinSegments;
};

struct PianomaniaStructuralAssignment
{
    std::vector<std::pair<int, int> > systems;
};

constexpr std::array<Grip, 4> PIANOMANIA_PRETTIFY_GRIPS = {
    Grip::START, Grip::BEZIER1, Grip::BEZIER2, Grip::END
};

constexpr std::array<Pid, 4> PIANOMANIA_PRETTIFY_SLUR_PROPERTIES = {
    Pid::SLUR_UOFF1, Pid::SLUR_UOFF2, Pid::SLUR_UOFF3, Pid::SLUR_UOFF4
};

constexpr double PIANOMANIA_PRETTIFY_CONVERGENCE_SP = 0.02;
constexpr double PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP = 0.02;

bool hasPianomaniaManualSlurEdit(const SlurSegment* slurSeg)
{
    return slurSeg && (slurSeg->isEdited() || !slurSeg->offset().isNull() || !slurSeg->autoplace());
}

bool hasManualFingeringPlacement(const Fingering* fingering)
{
    return fingering && (!fingering->autoplace()
                         || !fingering->isStyled(Pid::OFFSET)
                         || !fingering->offset().isNull()
                         || !fingering->isStyled(Pid::MIN_DISTANCE)
                         || fingering->propertyFlags(Pid::PLACEMENT) != PropertyFlags::STYLED);
}

void collectPianomaniaPrettifyFingerings(void* data, EngravingItem* item)
{
    if (!item || !item->isFingering()) {
        return;
    }

    auto* targets = static_cast<PianomaniaPrettifyTargets*>(data);
    Fingering* fingering = toFingering(item);
    PianomaniaFingeringTarget target;
    target.fingering = fingering;
    target.pos = item->pos();
    target.placement = fingering->placement();
    target.flagsOnOffset = fingering->offset();
    target.flagsOnOffsetFlags = fingering->propertyFlags(Pid::OFFSET);
    target.flagsOnMinDistance = fingering->minDistance().val();
    target.flagsOnMinDistanceFlags = fingering->propertyFlags(Pid::MIN_DISTANCE);
    targets->fingerings.push_back(target);
}

void collectPianomaniaPrettifySnapshotFingerings(void* data, EngravingItem* item)
{
    if (!item || !item->isFingering()) {
        return;
    }

    auto* snapshot = static_cast<PianomaniaPrettifySnapshot*>(data);
    Fingering* fingering = toFingering(item);
    snapshot->fingerings.push_back(PianomaniaFingeringSnapshotEntry {
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

PianomaniaPrettifyTargets collectPianomaniaPrettifyTargets(Score* score)
{
    PianomaniaPrettifyTargets targets;
    if (!score) {
        return targets;
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

            PianomaniaSlurSegmentTarget target;
            target.slur = slur;
            target.segmentIndex = i;
            for (size_t gripIndex = 0; gripIndex < PIANOMANIA_PRETTIFY_GRIPS.size(); ++gripIndex) {
                target.gripPositions[gripIndex] = segment->ups(PIANOMANIA_PRETTIFY_GRIPS[gripIndex]).pos();
            }
            targets.slurSegments.push_back(target);
        }
    }

    score->scanElements(&targets, collectPianomaniaPrettifyFingerings, true);

    for (const auto& pair : score->spanner()) {
        Spanner* spanner = pair.second;
        if (!spanner || !spanner->isHairpin()) {
            continue;
        }

        Hairpin* hairpin = toHairpin(spanner);
        for (size_t i = 0; i < hairpin->nsegments(); ++i) {
            SpannerSegment* segment = hairpin->segmentAt(static_cast<int>(i));
            if (!segment) {
                continue;
            }
            targets.hairpinSegments.push_back(PianomaniaHairpinSegmentTarget { hairpin, i, segment->pos().y() });
        }
    }

    return targets;
}

PianomaniaPrettifySnapshot collectPianomaniaPrettifySnapshot(Score* score)
{
    PianomaniaPrettifySnapshot snapshot;
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

            PianomaniaSlurSnapshotEntry entry;
            entry.slur = slur;
            entry.segmentIndex = i;
            for (size_t gripIndex = 0; gripIndex < PIANOMANIA_PRETTIFY_GRIPS.size(); ++gripIndex) {
                const Grip grip = PIANOMANIA_PRETTIFY_GRIPS[gripIndex];
                const Pid property = PIANOMANIA_PRETTIFY_SLUR_PROPERTIES[gripIndex];
                entry.offsets[gripIndex] = segment->ups(grip).off;
                entry.offsetFlags[gripIndex] = segment->propertyFlags(property);
            }
            entry.offset = segment->offset();
            entry.generalOffsetFlags = segment->propertyFlags(Pid::OFFSET);
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

    score->scanElements(&snapshot, collectPianomaniaPrettifySnapshotFingerings, true);

    for (const auto& pair : score->spanner()) {
        Spanner* spanner = pair.second;
        if (!spanner || !spanner->isHairpin()) {
            continue;
        }

        Hairpin* hairpin = toHairpin(spanner);
        for (size_t i = 0; i < hairpin->nsegments(); ++i) {
            SpannerSegment* segment = hairpin->segmentAt(static_cast<int>(i));
            if (!segment) {
                continue;
            }

            PianomaniaHairpinSnapshotEntry entry;
            entry.hairpin = hairpin;
            entry.segmentIndex = i;
            entry.offset = segment->offset();
            entry.offsetFlags = segment->propertyFlags(Pid::OFFSET);
            entry.offsetChanged = segment->ldata()->offsetChanged();
            entry.changedPos = segment->ldata()->autoplace.changedPos;
            entry.spatium = std::max(1.0, segment->spatium());
            snapshot.hairpinSegments.push_back(entry);
        }
    }

    return snapshot;
}

bool pointNear(const PointF& a, const PointF& b, double tolerance)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y()) <= tolerance;
}

bool snapshotsEquivalent(const PianomaniaPrettifySnapshot& a, const PianomaniaPrettifySnapshot& b)
{
    if (a.slurSegments.size() != b.slurSegments.size() || a.fingerings.size() != b.fingerings.size()
        || a.hairpinSegments.size() != b.hairpinSegments.size()) {
        return false;
    }

    for (size_t i = 0; i < a.slurSegments.size(); ++i) {
        const PianomaniaSlurSnapshotEntry& left = a.slurSegments[i];
        const PianomaniaSlurSnapshotEntry& right = b.slurSegments[i];
        if (left.slur != right.slur
            || left.segmentIndex != right.segmentIndex
            || left.generalOffsetFlags != right.generalOffsetFlags
            || left.autoplace != right.autoplace
            || left.autoplaceFlags != right.autoplaceFlags
            || left.offsetChanged != right.offsetChanged
            || std::abs(left.extraHeight - right.extraHeight) > PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP
            || !pointNear(left.offset, right.offset, PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP * left.spatium)) {
            return false;
        }
        if (!pointNear(left.endPointOff1, right.endPointOff1, PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP * left.spatium)
            || !pointNear(left.endPointOff2, right.endPointOff2, PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP * left.spatium)
            || !pointNear(left.changedPos, right.changedPos, PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP * left.spatium)) {
            return false;
        }

        for (size_t gripIndex = 0; gripIndex < PIANOMANIA_PRETTIFY_GRIPS.size(); ++gripIndex) {
            if (left.offsetFlags[gripIndex] != right.offsetFlags[gripIndex]
                || !pointNear(left.offsets[gripIndex], right.offsets[gripIndex],
                              PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP * left.spatium)) {
                return false;
            }
        }
    }

    for (size_t i = 0; i < a.fingerings.size(); ++i) {
        const PianomaniaFingeringSnapshotEntry& left = a.fingerings[i];
        const PianomaniaFingeringSnapshotEntry& right = b.fingerings[i];
        const double tolerance = PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP * left.spatium;
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
            || std::abs(left.minDistance - right.minDistance) > PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP) {
            return false;
        }
    }

    for (size_t i = 0; i < a.hairpinSegments.size(); ++i) {
        const PianomaniaHairpinSnapshotEntry& left = a.hairpinSegments[i];
        const PianomaniaHairpinSnapshotEntry& right = b.hairpinSegments[i];
        const double tolerance = PIANOMANIA_PRETTIFY_SNAPSHOT_TOLERANCE_SP * left.spatium;
        if (left.hairpin != right.hairpin
            || left.segmentIndex != right.segmentIndex
            || left.offsetFlags != right.offsetFlags
            || left.offsetChanged != right.offsetChanged
            || !pointNear(left.offset, right.offset, tolerance)
            || !pointNear(left.changedPos, right.changedPos, tolerance)) {
            return false;
        }
    }

    return true;
}

void restorePianomaniaPrettifySnapshot(const PianomaniaPrettifySnapshot& snapshot)
{
    for (const PianomaniaSlurSnapshotEntry& entry : snapshot.slurSegments) {
        if (!entry.slur || entry.segmentIndex >= entry.slur->nsegments()) {
            continue;
        }

        SlurSegment* segment = entry.slur->segmentAt(static_cast<int>(entry.segmentIndex));
        if (!segment) {
            continue;
        }

        for (size_t gripIndex = 0; gripIndex < PIANOMANIA_PRETTIFY_GRIPS.size(); ++gripIndex) {
            const Pid property = PIANOMANIA_PRETTIFY_SLUR_PROPERTIES[gripIndex];
            segment->setProperty(property, entry.offsets[gripIndex]);
            segment->setPropertyFlags(property, entry.offsetFlags[gripIndex]);
        }

        segment->setProperty(Pid::OFFSET, entry.offset);
        segment->setPropertyFlags(Pid::OFFSET, entry.generalOffsetFlags);
        segment->setProperty(Pid::AUTOPLACE, entry.autoplace);
        segment->setPropertyFlags(Pid::AUTOPLACE, entry.autoplaceFlags);
        segment->setEndPointOff1(entry.endPointOff1);
        segment->setEndPointOff2(entry.endPointOff2);
        segment->setExtraHeight(entry.extraHeight);
        segment->mutldata()->autoplace.offsetChanged = entry.offsetChanged;
        segment->mutldata()->autoplace.changedPos = entry.changedPos;
    }

    for (const PianomaniaFingeringSnapshotEntry& entry : snapshot.fingerings) {
        Fingering* fingering = entry.fingering;
        if (!fingering) {
            continue;
        }

        fingering->setProperty(Pid::PLACEMENT, entry.placement);
        fingering->setPropertyFlags(Pid::PLACEMENT, entry.placementFlags);
        fingering->setProperty(Pid::OFFSET, entry.offset);
        fingering->setPropertyFlags(Pid::OFFSET, entry.offsetFlags);
        fingering->setProperty(Pid::MIN_DISTANCE, Spatium(entry.minDistance));
        fingering->setPropertyFlags(Pid::MIN_DISTANCE, entry.minDistanceFlags);
        fingering->setProperty(Pid::AUTOPLACE, entry.autoplace);
        fingering->setPropertyFlags(Pid::AUTOPLACE, entry.autoplaceFlags);
        fingering->mutldata()->autoplace.offsetChanged = entry.offsetChanged;
        fingering->mutldata()->autoplace.changedPos = entry.changedPos;
    }

    for (const PianomaniaHairpinSnapshotEntry& entry : snapshot.hairpinSegments) {
        if (!entry.hairpin || entry.segmentIndex >= entry.hairpin->nsegments()) {
            continue;
        }

        SpannerSegment* segment = entry.hairpin->segmentAt(static_cast<int>(entry.segmentIndex));
        if (!segment) {
            continue;
        }

        segment->setProperty(Pid::OFFSET, entry.offset);
        segment->setPropertyFlags(Pid::OFFSET, entry.offsetFlags);
        segment->mutldata()->autoplace.offsetChanged = entry.offsetChanged;
        segment->mutldata()->autoplace.changedPos = entry.changedPos;
    }
}

PianomaniaStructuralAssignment pianomaniaStructuralAssignment(Score* score)
{
    PianomaniaStructuralAssignment assignment;
    if (!score) {
        return assignment;
    }

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
        assignment.systems.emplace_back(pageIndex, firstMeasure->tick().ticks());
    }

    return assignment;
}

bool structuralAssignmentsEquivalent(const PianomaniaStructuralAssignment& a, const PianomaniaStructuralAssignment& b)
{
    return a.systems == b.systems;
}

void warnIfPianomaniaPrettifyChangedStructure(const PianomaniaStructuralAssignment& before,
                                              const PianomaniaStructuralAssignment& after)
{
    if (structuralAssignmentsEquivalent(before, after)) {
        const int pageCount = before.systems.empty() ? 0 : before.systems.back().first + 1;
        LOGI() << "Pianomania prettify: structural assignment unchanged (" << before.systems.size() << " systems, "
               << pageCount << " pages)";
        return;
    }

    LOGW() << "Pianomania prettify changed the structural layout: systems " << before.systems.size()
           << " -> " << after.systems.size();
    const size_t n = std::max(before.systems.size(), after.systems.size());
    for (size_t i = 0; i < n; ++i) {
        const std::pair<int, int> b = i < before.systems.size() ? before.systems[i] : std::make_pair(-1, -1);
        const std::pair<int, int> a = i < after.systems.size() ? after.systems[i] : std::make_pair(-1, -1);
        if (b != a) {
            LOGW() << "  system " << i << ": page " << b.first << " tick " << b.second
                   << " -> page " << a.first << " tick " << a.second;
        }
    }
}

void resetSlurSegmentForPianomaniaPrettify(SlurSegment* segment, PmPrettifyResult& result)
{
    if (!segment) {
        return;
    }

    if (hasPianomaniaManualSlurEdit(segment)) {
        ++result.normalizedManualSlurs;
    }

    for (size_t gripIndex = 0; gripIndex < PIANOMANIA_PRETTIFY_GRIPS.size(); ++gripIndex) {
        const Grip grip = PIANOMANIA_PRETTIFY_GRIPS[gripIndex];
        const Pid property = PIANOMANIA_PRETTIFY_SLUR_PROPERTIES[gripIndex];
        if (!segment->ups(grip).off.isNull()) {
            segment->undoChangeProperty(property, PointF(), segment->propertyFlags(property));
        }
    }
    if (!segment->offset().isNull() || segment->propertyFlags(Pid::OFFSET) != PropertyFlags::STYLED) {
        segment->undoChangeProperty(Pid::OFFSET, PointF(), PropertyFlags::STYLED);
    }
    if (!segment->autoplace() || segment->propertyFlags(Pid::AUTOPLACE) != PropertyFlags::STYLED) {
        segment->undoChangeProperty(Pid::AUTOPLACE, true, PropertyFlags::STYLED);
    }
}

void resetFingeringForPianomaniaPrettify(Fingering* fingering, PmPrettifyResult& result)
{
    if (!fingering) {
        return;
    }

    if (hasManualFingeringPlacement(fingering)) {
        ++result.normalizedManualFingerings;
    }

    const PlacementV defaultPlacement = fingering->calculatePlacement();
    if (fingering->placement() != defaultPlacement || fingering->propertyFlags(Pid::PLACEMENT) != PropertyFlags::STYLED) {
        fingering->undoChangeProperty(Pid::PLACEMENT, defaultPlacement, PropertyFlags::STYLED);
    }
    if (!fingering->offset().isNull() || fingering->propertyFlags(Pid::OFFSET) != PropertyFlags::STYLED) {
        fingering->undoChangeProperty(Pid::OFFSET, PointF(), PropertyFlags::STYLED);
    }
    const Spatium defaultMinDistance = fingering->propertyDefault(Pid::MIN_DISTANCE).value<Spatium>();
    if (!fingering->isStyled(Pid::MIN_DISTANCE)
        || std::abs(fingering->minDistance().val() - defaultMinDistance.val()) > 0.0001) {
        fingering->undoChangeProperty(Pid::MIN_DISTANCE, fingering->propertyDefault(Pid::MIN_DISTANCE), PropertyFlags::STYLED);
    }
    if (!fingering->autoplace() || fingering->propertyFlags(Pid::AUTOPLACE) != PropertyFlags::STYLED) {
        fingering->undoChangeProperty(Pid::AUTOPLACE, true, PropertyFlags::STYLED);
    }
    fingering->setOffsetChanged(false);
}

void collectPianomaniaPrettifyNeutralizeFingerings(void* data, EngravingItem* item)
{
    if (!item || !item->isFingering()) {
        return;
    }

    auto* result = static_cast<PmPrettifyResult*>(data);
    resetFingeringForPianomaniaPrettify(toFingering(item), *result);
}

void neutralizePianomaniaPrettifyProperties(Score* score, const PmPrettifyOptions& options, PmPrettifyResult& result)
{
    if (!score || !options.forceNormalizeManual) {
        return;
    }

    for (const auto& pair : score->spanner()) {
        Spanner* spanner = pair.second;
        if (!spanner) {
            continue;
        }
        if (spanner->isSlur()) {
            Slur* slur = toSlur(spanner);
            for (size_t i = 0; i < slur->nsegments(); ++i) {
                resetSlurSegmentForPianomaniaPrettify(slur->segmentAt(static_cast<int>(i)), result);
            }
        } else if (spanner->isHairpin()) {
            Hairpin* hairpin = toHairpin(spanner);
            for (size_t i = 0; i < hairpin->nsegments(); ++i) {
                SpannerSegment* segment = hairpin->segmentAt(static_cast<int>(i));
                if (!segment) {
                    continue;
                }
                if (!segment->offset().isNull() || segment->propertyFlags(Pid::OFFSET) != PropertyFlags::STYLED) {
                    segment->undoChangeProperty(Pid::OFFSET, hairpin->propertyDefault(Pid::OFFSET), PropertyFlags::STYLED);
                }
            }
        }
    }

    score->scanElements(&result, collectPianomaniaPrettifyNeutralizeFingerings, true);
}

// Only fingerings the prettify pass owns may be pinned by the replay sentinel;
// manual fingerings keep their own autoplace behaviour. In force mode the
// neutralize step reset every fingering, so all of them are owned. In
// non-force mode ownership is judged against the neutral snapshot (taken from
// a layout without the prettify pass, so manual properties there are genuine).
void markPianomaniaPrettifyControlledFingerings(PianomaniaPrettifyTargets& targets,
                                                const PianomaniaPrettifySnapshot& neutralSnapshot,
                                                bool forceNormalizeManual)
{
    if (forceNormalizeManual) {
        for (PianomaniaFingeringTarget& target : targets.fingerings) {
            target.pmControlled = true;
        }
        return;
    }

    std::unordered_set<const Fingering*> pmControlled;
    for (const PianomaniaFingeringSnapshotEntry& entry : neutralSnapshot.fingerings) {
        const bool manual = !entry.autoplace
                            || !entry.offset.isNull()
                            || entry.offsetFlags != PropertyFlags::STYLED
                            || entry.minDistanceFlags != PropertyFlags::STYLED
                            || entry.placementFlags != PropertyFlags::STYLED;
        if (!manual) {
            pmControlled.insert(entry.fingering);
        }
    }
    for (PianomaniaFingeringTarget& target : targets.fingerings) {
        target.pmControlled = pmControlled.count(target.fingering) > 0;
    }
}

void applyPianomaniaPrettifyTargetPlacements(const PianomaniaPrettifyTargets& targets)
{
    for (const PianomaniaFingeringTarget& target : targets.fingerings) {
        Fingering* fingering = target.fingering;
        if (!fingering) {
            continue;
        }
        if (fingering->placement() != target.placement
            || fingering->propertyFlags(Pid::PLACEMENT) != PropertyFlags::STYLED) {
            fingering->undoChangeProperty(Pid::PLACEMENT, target.placement, PropertyFlags::STYLED);
        }
        // Re-seed the flags-on Autoplace rebases (offset + min-distance) for
        // every prettify-owned digit before the delta loop measures its first
        // baseline, so every layout of the replayed score places the digit
        // correctly before the skylines are built (see PianomaniaFingeringTarget).
        if (!target.pmControlled) {
            continue;
        }
        bool touched = false;
        if (fingering->offset() != target.flagsOnOffset
            || fingering->propertyFlags(Pid::OFFSET) != target.flagsOnOffsetFlags) {
            fingering->undoChangeProperty(Pid::OFFSET, target.flagsOnOffset, target.flagsOnOffsetFlags);
            touched = true;
        }
        if (fingering->minDistance().val() != target.flagsOnMinDistance
            || fingering->propertyFlags(Pid::MIN_DISTANCE) != target.flagsOnMinDistanceFlags) {
            fingering->undoChangeProperty(Pid::MIN_DISTANCE, Spatium(target.flagsOnMinDistance),
                                          target.flagsOnMinDistanceFlags);
            touched = true;
        }
        if (touched) {
            fingering->setOffsetChanged(false);
        }
    }
}

bool applyPianomaniaPrettifyTargetDeltas(const PianomaniaPrettifyTargets& targets)
{
    bool changed = false;

    for (const PianomaniaSlurSegmentTarget& target : targets.slurSegments) {
        if (!target.slur || target.segmentIndex >= target.slur->nsegments()) {
            continue;
        }

        SlurSegment* segment = target.slur->segmentAt(static_cast<int>(target.segmentIndex));
        if (!segment) {
            continue;
        }

        for (size_t gripIndex = 0; gripIndex < PIANOMANIA_PRETTIFY_GRIPS.size(); ++gripIndex) {
            const Grip grip = PIANOMANIA_PRETTIFY_GRIPS[gripIndex];
            const PointF baselinePos = segment->ups(grip).pos();
            const PointF delta = target.gripPositions[gripIndex] - baselinePos;
            if (delta.isNull()) {
                continue;
            }

            const double sp = segment->spatium();
            const Pid property = PIANOMANIA_PRETTIFY_SLUR_PROPERTIES[gripIndex];
            const PointF prettifiedOffset = segment->ups(grip).off + delta;
            segment->undoChangeProperty(property, prettifiedOffset, segment->propertyFlags(property));
            changed = changed || std::hypot(delta.x(), delta.y()) > PIANOMANIA_PRETTIFY_CONVERGENCE_SP * sp;
        }
    }

    for (const PianomaniaFingeringTarget& target : targets.fingerings) {
        Fingering* fingering = target.fingering;
        if (!fingering) {
            continue;
        }

        const PointF delta = target.pos - fingering->pos();
        if (delta.isNull()) {
            continue;
        }

        const double sp = fingering->spatium();
        fingering->undoChangeProperty(Pid::OFFSET, fingering->offset() + delta, PropertyFlags::UNSTYLED);
        fingering->undoChangeProperty(Pid::MIN_DISTANCE, -999.0, PropertyFlags::UNSTYLED);
        fingering->setOffsetChanged(false);
        changed = changed || std::hypot(delta.x(), delta.y()) > PIANOMANIA_PRETTIFY_CONVERGENCE_SP * sp;
    }

    for (const PianomaniaHairpinSegmentTarget& target : targets.hairpinSegments) {
        if (!target.hairpin || target.segmentIndex >= target.hairpin->nsegments()) {
            continue;
        }

        SpannerSegment* segment = target.hairpin->segmentAt(static_cast<int>(target.segmentIndex));
        if (!segment) {
            continue;
        }

        const double deltaY = target.posY - segment->pos().y();
        if (muse::RealIsNull(deltaY)) {
            continue;
        }

        const double sp = segment->spatium();
        // Replay converges because autoplaceSpannerSegment only ever pushes a
        // segment away from the staff: an away-from-staff offset is preserved
        // verbatim by the next layout.
        segment->undoChangeProperty(Pid::OFFSET, segment->offset() + PointF(0.0, deltaY), PropertyFlags::UNSTYLED);
        segment->setOffsetChanged(false);
        changed = changed || std::abs(deltaY) > PIANOMANIA_PRETTIFY_CONVERGENCE_SP * sp;
    }

    return changed;
}

void applyPianomaniaPrettifyTargets(Score* score, const PianomaniaPrettifyTargets& targets)
{
    if (!score) {
        return;
    }

    static constexpr int maxCorrectionPasses = 6;
    for (int i = 0; i < maxCorrectionPasses; ++i) {
        if (!applyPianomaniaPrettifyTargetDeltas(targets)) {
            break;
        }

        score->setLayoutAll();
        score->doLayout();
    }
}

} // namespace

PmPrettifyResult mu::engraving::pm::applyPianomaniaPrettify(Score* score, const PmPrettifyOptions& options)
{
    PmPrettifyResult result;
    if (!score) {
        return result;
    }

    score->setLayoutAll();
    score->doLayout();

    const PianomaniaStructuralAssignment structureBefore = pianomaniaStructuralAssignment(score);
    const PianomaniaPrettifySnapshot snapshotBefore = collectPianomaniaPrettifySnapshot(score);

    neutralizePianomaniaPrettifyProperties(score, options, result);
    score->setLayoutAll();
    score->doLayout();
    const PianomaniaPrettifySnapshot neutralSnapshot = collectPianomaniaPrettifySnapshot(score);

    const bool previousPrettify = MScore::pianomaniaPrettifySlursFingerings;
    const bool previousForceNormalize = MScore::pianomaniaForceNormalizeSlursFingerings;
    PianomaniaPrettifyTargets prettifyTargets;

    MScore::pianomaniaPrettifySlursFingerings = true;
    MScore::pianomaniaForceNormalizeSlursFingerings = false;
    MScore::resetPianomaniaSlurFingeringDiagnostics();

    score->setLayoutAll();
    score->doLayout();
    prettifyTargets = collectPianomaniaPrettifyTargets(score);
    markPianomaniaPrettifyControlledFingerings(prettifyTargets, neutralSnapshot, options.forceNormalizeManual);
    result.manualReviewFingerings = MScore::pianomaniaManualReviewFingerings;

    MScore::pianomaniaPrettifySlursFingerings = previousPrettify;
    MScore::pianomaniaForceNormalizeSlursFingerings = previousForceNormalize;

    restorePianomaniaPrettifySnapshot(neutralSnapshot);
    applyPianomaniaPrettifyTargetPlacements(prettifyTargets);
    score->setLayoutAll();
    score->doLayout();
    applyPianomaniaPrettifyTargets(score, prettifyTargets);

    const PianomaniaStructuralAssignment structureAfter = pianomaniaStructuralAssignment(score);
    result.structuralAssignmentChanged = !structuralAssignmentsEquivalent(structureBefore, structureAfter);
    warnIfPianomaniaPrettifyChangedStructure(structureBefore, structureAfter);

    const PianomaniaPrettifySnapshot snapshotAfter = collectPianomaniaPrettifySnapshot(score);
    result.changed = !snapshotsEquivalent(snapshotBefore, snapshotAfter);
    if (!result.changed || result.structuralAssignmentChanged) {
        result.normalizedManualSlurs = 0;
        result.normalizedManualFingerings = 0;
        result.manualReviewFingerings = 0;
    }

    MScore::pianomaniaNormalizedManualSlurs = result.normalizedManualSlurs;
    MScore::pianomaniaNormalizedManualFingerings = result.normalizedManualFingerings;
    MScore::pianomaniaManualReviewFingerings = result.manualReviewFingerings;

    return result;
}
