/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */
#pragma once

namespace mu::engraving {
class MasterScore;
class Score;
}

namespace mu::engraving::pm {

struct PmAutoLayoutOptions
{
    int targetSystemsPerPage = 3;
    int maxVerifyPasses = 5;
    int maxPagePlanPasses = 10;
    // Page-fill headroom (inches) used only while planning page breaks, so the
    // plan survives the extra skyline growth that Prettify persists later
    // (fingering nudges around cross-staff slurs happen after page stacking
    // and only enter the skylines once their offsets are persisted).
    double pagePlanBottomReserveIn = 0.3;
};

void resetStructuralLayout(Score* score);
void applyPianomaniaAutoLayout(MasterScore* score, const PmAutoLayoutOptions& options = {});

}
