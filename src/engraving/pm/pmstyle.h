/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */
#pragma once

#include <unordered_map>

#include "../dom/mscore.h"
#include "../style/styledef.h"

namespace mu::engraving {
class Score;
}

namespace mu::engraving::pm {

struct PmPageGeometry
{
    static constexpr double pageWidthIn = 10.0;
    static constexpr double pageHeightIn = 7.5;
    static constexpr double pageMarginIn = 0.591;
    static constexpr double spatiumIn = 0.07;
    static constexpr double spatiumLayoutUnits = spatiumIn * DPI;
};

std::unordered_map<Sid, PropertyValue> pianomaniaStyleValues();

bool enforcePianomaniaStyle(Score* score);
void applyPianomaniaStyle(Score* score);
void stripHeaderFramesAndFooters(Score* score);

}
