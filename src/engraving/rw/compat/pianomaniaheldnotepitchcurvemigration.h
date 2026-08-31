/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "types/string.h"
#include "../../types/pianomaniaheldnotepitchcurve.h"

namespace mu::engraving {
class Note;
class Score;
class XmlReader;

namespace compat {

class PianomaniaHeldNotePitchCurveMigration
{
public:
    struct RawPoint {
        int64_t time = 0;
        int64_t pitch = 0;
        int64_t slope = 0;

        bool operator==(const RawPoint& other) const
        {
            return time == other.time && pitch == other.pitch && slope == other.slope;
        }
    };
    using RawCurve = std::vector<RawPoint>;

    void read(Note* note, XmlReader& xml);
    bool migrate(Score* score, muse::String* error);

    static bool migrateCurve(const RawCurve& rawCurve, int durationTicks,
                             PianomaniaHeldNotePitchCurve* migratedCurve,
                             muse::String* error);

private:
    std::unordered_map<Note*, RawCurve> m_stagedVersion2Curves;
    std::unordered_map<Note*, PianomaniaHeldNotePitchCurve> m_stagedVersion3Curves;
};

}
}
