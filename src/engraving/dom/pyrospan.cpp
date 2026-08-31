/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited
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

#include "pyrospan.h"

#include "score.h"
#include "system.h"

using namespace mu;
using namespace mu::engraving;

namespace mu::engraving {
static const Color PYRO_SPAN_COLOR(255, 122, 26); // orange

static const ElementStyle pyroSpanStyle {
    { Sid::letRingFontFace,                      Pid::BEGIN_FONT_FACE },
    { Sid::letRingFontFace,                      Pid::CONTINUE_FONT_FACE },
    { Sid::letRingFontFace,                      Pid::END_FONT_FACE },
    { Sid::letRingFontSize,                      Pid::BEGIN_FONT_SIZE },
    { Sid::letRingFontSize,                      Pid::CONTINUE_FONT_SIZE },
    { Sid::letRingFontSize,                      Pid::END_FONT_SIZE },
    { Sid::letRingFontStyle,                     Pid::BEGIN_FONT_STYLE },
    { Sid::letRingFontStyle,                     Pid::CONTINUE_FONT_STYLE },
    { Sid::letRingFontStyle,                     Pid::END_FONT_STYLE },
    { Sid::letRingTextAlign,                     Pid::BEGIN_TEXT_ALIGN },
    { Sid::letRingTextAlign,                     Pid::CONTINUE_TEXT_ALIGN },
    { Sid::letRingTextAlign,                     Pid::END_TEXT_ALIGN },
    { Sid::letRingFontSpatiumDependent,          Pid::TEXT_SIZE_SPATIUM_DEPENDENT },
};

PyroSpanSegment::PyroSpanSegment(PyroSpan* sp, System* parent)
    : TextLineBaseSegment(ElementType::PYRO_SPAN_SEGMENT, sp, parent, ElementFlag::MOVABLE | ElementFlag::ON_STAFF)
{
}

//---------------------------------------------------------
//   PyroSpan
//---------------------------------------------------------

PyroSpan::PyroSpan(EngravingItem* parent)
    : ChordTextLineBase(ElementType::PYRO_SPAN, parent)
{
    initElementStyle(&pyroSpanStyle);
    resetProperty(Pid::LINE_VISIBLE);
    resetProperty(Pid::LINE_WIDTH);
    resetProperty(Pid::LINE_STYLE);
    resetProperty(Pid::COLOR);
    resetProperty(Pid::PLACEMENT);

    resetProperty(Pid::BEGIN_HOOK_TYPE);
    resetProperty(Pid::BEGIN_HOOK_HEIGHT);
    resetProperty(Pid::END_HOOK_TYPE);
    resetProperty(Pid::END_HOOK_HEIGHT);

    resetProperty(Pid::BEGIN_TEXT_PLACE);
    resetProperty(Pid::BEGIN_TEXT);
    resetProperty(Pid::CONTINUE_TEXT_PLACE);
    resetProperty(Pid::CONTINUE_TEXT);
    resetProperty(Pid::END_TEXT_PLACE);
    resetProperty(Pid::END_TEXT);
}

static const ElementStyle pyroSpanSegmentStyle {
    { Sid::letRingMinDistance,    Pid::MIN_DISTANCE },
};

//---------------------------------------------------------
//   createLineSegment
//---------------------------------------------------------

LineSegment* PyroSpan::createLineSegment(System* parent)
{
    PyroSpanSegment* ps = new PyroSpanSegment(this, parent);
    ps->setTrack(track());
    ps->initElementStyle(&pyroSpanSegmentStyle);
    return ps;
}

//---------------------------------------------------------
//   propertyDefault
//---------------------------------------------------------

PropertyValue PyroSpan::propertyDefault(Pid propertyId) const
{
    switch (propertyId) {
    case Pid::COLOR:
        return PropertyValue::fromValue(PYRO_SPAN_COLOR);

    case Pid::PLACEMENT:
        return PlacementV::ABOVE;

    case Pid::LINE_WIDTH:
        return Spatium(0.25);

    case Pid::LINE_STYLE:
        return LineType::SOLID;

    case Pid::LINE_VISIBLE:
        return true;

    case Pid::ALIGN:
        return Align(AlignH::LEFT, AlignV::BASELINE);

    // Down-turned 90-degree hooks at both ends: the top halves of
    // "[" and "]" bracketing the span from above.
    case Pid::BEGIN_HOOK_TYPE:
    case Pid::END_HOOK_TYPE:
        return HookType::HOOK_90;

    case Pid::BEGIN_HOOK_HEIGHT:
    case Pid::END_HOOK_HEIGHT:
        return Spatium(1.2);

    case Pid::BEGIN_TEXT_OFFSET:
    case Pid::CONTINUE_TEXT_OFFSET:
    case Pid::END_TEXT_OFFSET:
        return PropertyValue::fromValue(PointF(0, 0));

    case Pid::BEGIN_TEXT:
    case Pid::CONTINUE_TEXT:
    case Pid::END_TEXT:
        return "";

    case Pid::BEGIN_TEXT_PLACE:
    case Pid::CONTINUE_TEXT_PLACE:
    case Pid::END_TEXT_PLACE:
        return TextPlace::AUTO;

    default:
        return TextLineBase::propertyDefault(propertyId);
    }
}
}
