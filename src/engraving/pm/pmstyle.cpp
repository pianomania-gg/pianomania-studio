/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */

#include "pmstyle.h"

#include <vector>

#include "../dom/measurebase.h"
#include "../dom/score.h"
#include "../editing/editsystemlocks.h"
#include "../types/dimension.h"

using namespace mu::engraving;
using namespace mu::engraving::pm;

namespace {

bool enforceStyleValue(Score* score, Sid sid, const PropertyValue& value)
{
    PropertyValue oldValue = score->style().styleV(sid);
    if (oldValue == value) {
        return false;
    }

    score->style().set(sid, value);

    switch (sid) {
    case Sid::spatium:
        score->spatiumChanged(oldValue.toDouble(), value.toDouble());
        break;
    case Sid::createMultiMeasureRests:
        if (oldValue.toBool() && !value.toBool()) {
            EditSystemLocks::removeSystemLocksContainingMMRests(score);
        }
        break;
    default:
        break;
    }

    return true;
}

}

std::unordered_map<Sid, PropertyValue> mu::engraving::pm::pianomaniaStyleValues()
{
    const double printableWidth = PmPageGeometry::pageWidthIn - (2.0 * PmPageGeometry::pageMarginIn);

    return {
        { Sid::pageWidth, PmPageGeometry::pageWidthIn },
        { Sid::pageHeight, PmPageGeometry::pageHeightIn },
        { Sid::pagePrintableWidth, printableWidth },
        { Sid::pageEvenLeftMargin, PmPageGeometry::pageMarginIn },
        { Sid::pageOddLeftMargin, PmPageGeometry::pageMarginIn },
        { Sid::pageEvenTopMargin, PmPageGeometry::pageMarginIn },
        { Sid::pageEvenBottomMargin, PmPageGeometry::pageMarginIn },
        { Sid::pageOddTopMargin, PmPageGeometry::pageMarginIn },
        { Sid::pageOddBottomMargin, PmPageGeometry::pageMarginIn },
        { Sid::pageTwosided, false },

        { Sid::spatium, PmPageGeometry::spatiumLayoutUnits },
        { Sid::showHeader, false },
        { Sid::showFooter, false },
        { Sid::hideInstrumentNameIfOneInstrument, true },
        { Sid::firstSystemInstNameVisibility, int(InstrumentLabelVisibility::HIDE) },
        { Sid::subsSystemInstNameVisibility, int(InstrumentLabelVisibility::HIDE) },
        { Sid::enableIndentationOnFirstSystem, false },
        { Sid::firstSystemIndentationValue, Spatium(0.0) },
        { Sid::fingeringFontFace, String(u"Academico") },
        { Sid::fingeringFontSize, 8.0 },
        { Sid::fingeringFontStyle, int(FontStyle::Bold) },

        { Sid::clefKeyDistance, Spatium(1.0) },
        { Sid::clefTimesigDistance, Spatium(1.0) },
        { Sid::keyTimesigDistance, Spatium(1.25) },
        { Sid::systemHeaderDistance, Spatium(2.5) },
        { Sid::systemHeaderTimeSigDistance, Spatium(2.5) },
        { Sid::barNoteDistance, Spatium(1.0) },
        { Sid::noteBarDistance, Spatium(1.0) },
        { Sid::minNoteDistance, Spatium(0.5) },
        { Sid::accidentalDistance, Spatium(0.25) },
        { Sid::accidentalNoteDistance, Spatium(0.25) },
        { Sid::barAccidentalDistance, Spatium(0.65) },
        { Sid::measureSpacing, 1.5 },
        { Sid::minMeasureWidth, Spatium(8.0) },

        { Sid::akkoladeDistance, Spatium(6.5) },
        { Sid::staffDistance, Spatium(6.5) },
        { Sid::minSystemDistance, Spatium(8.5) },
        { Sid::maxSystemDistance, Spatium(13.0) },
        { Sid::enableVerticalSpread, true },
        { Sid::spreadSystem, 2.5 },
        { Sid::minSystemSpread, Spatium(8.5) },
        { Sid::maxSystemSpread, Spatium(24.0) },
        { Sid::minStaffSpread, Spatium(5.5) },
        { Sid::maxAkkoladeDistance, Spatium(8.5) },
        { Sid::maxPageFillSpread, Spatium(6.0) },
        { Sid::lastSystemFillLimit, 0.3 },

        { Sid::showMeasureNumber, true },
        { Sid::showMeasureNumberOne, false },
        { Sid::measureNumberSystem, true },

        { Sid::hideEmptyStaves, false },
        { Sid::createMultiMeasureRests, false },
    };
}

bool mu::engraving::pm::enforcePianomaniaStyle(Score* score)
{
    if (!score) {
        return false;
    }

    bool changed = false;
    for (const auto& [sid, value] : pianomaniaStyleValues()) {
        changed = enforceStyleValue(score, sid, value) || changed;
    }

    if (changed) {
        score->styleChanged();
    }

    return changed;
}

void mu::engraving::pm::applyPianomaniaStyle(Score* score)
{
    if (!score) {
        return;
    }

    score->undoChangeStyleValues(pianomaniaStyleValues());
}

void mu::engraving::pm::stripHeaderFramesAndFooters(Score* score)
{
    if (!score) {
        return;
    }

    std::vector<EngravingItem*> framesToRemove;
    bool seenMeasure = false;
    for (MeasureBase* mb = score->first(); mb; mb = mb->next()) {
        if (mb->isMeasure()) {
            seenMeasure = true;
            continue;
        }

        if (!seenMeasure && (mb->isVBoxBase() || mb->isHBox())) {
            framesToRemove.push_back(mb);
            continue;
        }

        if (seenMeasure && mb->isHBox() && !mb->nextMeasure()) {
            framesToRemove.push_back(mb);
        }
    }

    for (EngravingItem* frame : framesToRemove) {
        score->undoRemoveElement(frame);
    }
}
