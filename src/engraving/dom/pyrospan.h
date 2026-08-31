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

#ifndef MU_ENGRAVING_PYROSPAN_H
#define MU_ENGRAVING_PYROSPAN_H

#include "chordtextlinebase.h"

namespace mu::engraving {
class PyroSpan;

//---------------------------------------------------------
//   @@ PyroSpanSegment
//---------------------------------------------------------

class PyroSpanSegment final : public TextLineBaseSegment
{
    OBJECT_ALLOCATOR(engraving, PyroSpanSegment)
    DECLARE_CLASSOF(ElementType::PYRO_SPAN_SEGMENT)

public:
    PyroSpanSegment(PyroSpan* sp, System* parent);

    PyroSpanSegment* clone() const override { return new PyroSpanSegment(*this); }

    PyroSpan* pyroSpan() const { return (PyroSpan*)spanner(); }

    friend class PyroSpan;
};

//---------------------------------------------------------
//   @@ PyroSpan
///   Pianomania: marks a note range where game-mode pyro cannons
///   fire from the screen edges. Rendered as an orange bracket line
///   above the staff (down-turned hooks at both ends).
//---------------------------------------------------------

class PyroSpan final : public ChordTextLineBase
{
    OBJECT_ALLOCATOR(engraving, PyroSpan)
    DECLARE_CLASSOF(ElementType::PYRO_SPAN)

public:
    PyroSpan(EngravingItem* parent);

    PyroSpan* clone() const override { return new PyroSpan(*this); }

    LineSegment* createLineSegment(System* parent) override;

    PropertyValue propertyDefault(Pid propertyId) const override;
};
} // namespace mu::engraving
#endif
