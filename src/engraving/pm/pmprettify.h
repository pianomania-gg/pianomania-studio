/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */
#pragma once

namespace mu::engraving {
class Score;
}

namespace mu::engraving::pm {

struct PmPrettifyOptions
{
    bool forceNormalizeManual = true;
};

struct PmPrettifyResult
{
    bool changed = false;
    bool structuralAssignmentChanged = false;
    int normalizedManualSlurs = 0;
    int normalizedManualFingerings = 0;
    int manualReviewFingerings = 0;
};

PmPrettifyResult applyPianomaniaPrettify(Score* score, const PmPrettifyOptions& options = {});

}
