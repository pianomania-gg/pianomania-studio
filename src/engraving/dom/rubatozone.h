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

#ifndef MU_ENGRAVING_RUBATOZONE_H
#define MU_ENGRAVING_RUBATOZONE_H

#include "chordtextlinebase.h"

namespace mu::engraving {
class RubatoZone;

//---------------------------------------------------------
//   @@ RubatoZoneSegment
//---------------------------------------------------------

class RubatoZoneSegment final : public TextLineBaseSegment
{
    OBJECT_ALLOCATOR(engraving, RubatoZoneSegment)
    DECLARE_CLASSOF(ElementType::RUBATO_ZONE_SEGMENT)

public:
    RubatoZoneSegment(RubatoZone* sp, System* parent);

    RubatoZoneSegment* clone() const override { return new RubatoZoneSegment(*this); }

    RubatoZone* rubatoZone() const { return (RubatoZone*)spanner(); }

    friend class RubatoZone;
};

//---------------------------------------------------------
//   @@ RubatoZone
///   Pianomania: marks a note range where game-mode timing is
///   player-driven instead of mechanical. Rendered as a purple
///   bracket line above the staff (down-turned hooks at both ends).
//---------------------------------------------------------

class RubatoZone final : public ChordTextLineBase
{
    OBJECT_ALLOCATOR(engraving, RubatoZone)
    DECLARE_CLASSOF(ElementType::RUBATO_ZONE)

public:
    RubatoZone(EngravingItem* parent);

    RubatoZone* clone() const override { return new RubatoZone(*this); }

    LineSegment* createLineSegment(System* parent) override;

    PropertyValue propertyDefault(Pid propertyId) const override;
};
} // namespace mu::engraving
#endif
