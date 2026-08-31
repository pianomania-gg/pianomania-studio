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

#ifndef MU_ENGRAVING_LASERSPAN_H
#define MU_ENGRAVING_LASERSPAN_H

#include "chordtextlinebase.h"

namespace mu::engraving {
class LaserSpan;

//---------------------------------------------------------
//   @@ LaserSpanSegment
//---------------------------------------------------------

class LaserSpanSegment final : public TextLineBaseSegment
{
    OBJECT_ALLOCATOR(engraving, LaserSpanSegment)
    DECLARE_CLASSOF(ElementType::LASER_SPAN_SEGMENT)

public:
    LaserSpanSegment(LaserSpan* sp, System* parent);

    LaserSpanSegment* clone() const override { return new LaserSpanSegment(*this); }

    LaserSpan* laserSpan() const { return (LaserSpan*)spanner(); }

    friend class LaserSpan;
};

//---------------------------------------------------------
//   @@ LaserSpan
///   Pianomania: marks a note range where game-mode laser sweeps
///   fan out from a rear projector. Rendered as a cyan bracket line
///   above the staff (down-turned hooks at both ends).
//---------------------------------------------------------

class LaserSpan final : public ChordTextLineBase
{
    OBJECT_ALLOCATOR(engraving, LaserSpan)
    DECLARE_CLASSOF(ElementType::LASER_SPAN)

public:
    LaserSpan(EngravingItem* parent);

    LaserSpan* clone() const override { return new LaserSpan(*this); }

    LineSegment* createLineSegment(System* parent) override;

    PropertyValue propertyDefault(Pid propertyId) const override;
};
} // namespace mu::engraving
#endif
