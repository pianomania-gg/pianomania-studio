/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */
#pragma once

#include <vector>
#ifndef NO_QT_SUPPORT
#include <QVariant>
#endif

namespace mu::engraving {

struct PianomaniaHeldNotePitchCurvePoint {
    static constexpr int MAX_PITCH_CENTS = 2400;
    static constexpr int MAX_SLOPE_CENTS_PER_QUARTER = 1000000;

    int scoreTick = 0;
    int pitchCents = 0;
    int slopeCentsPerQuarter = 0;

    PianomaniaHeldNotePitchCurvePoint() = default;
    PianomaniaHeldNotePitchCurvePoint(int tick, int pitch, int slope = 0)
        : scoreTick(tick), pitchCents(pitch), slopeCentsPerQuarter(slope) {}

    bool operator==(const PianomaniaHeldNotePitchCurvePoint& other) const
    {
        return scoreTick == other.scoreTick && pitchCents == other.pitchCents
               && slopeCentsPerQuarter == other.slopeCentsPerQuarter;
    }
    bool operator!=(const PianomaniaHeldNotePitchCurvePoint& other) const { return !(*this == other); }

#ifndef NO_QT_SUPPORT
    QVariant toQVariant() const
    {
        return QVariantMap { { "scoreTick", scoreTick }, { "pitch", pitchCents }, { "slope", slopeCentsPerQuarter } };
    }

    static PianomaniaHeldNotePitchCurvePoint fromQVariant(const QVariant& value)
    {
        const QVariantMap map = value.toMap();
        return { map.value("scoreTick").toInt(), map.value("pitch").toInt(), map.value("slope").toInt() };
    }
#endif
};

using PianomaniaHeldNotePitchCurve = std::vector<PianomaniaHeldNotePitchCurvePoint>;

#ifndef NO_QT_SUPPORT
inline QVariant pianomaniaHeldNotePitchCurveToQVariant(const PianomaniaHeldNotePitchCurve& curve)
{
    QVariantList result;
    result.reserve(static_cast<qsizetype>(curve.size()));
    for (const PianomaniaHeldNotePitchCurvePoint& point : curve) {
        result.append(point.toQVariant());
    }
    return result;
}

inline PianomaniaHeldNotePitchCurve pianomaniaHeldNotePitchCurveFromQVariant(const QVariant& value)
{
    PianomaniaHeldNotePitchCurve result;
    const QVariantList list = value.toList();
    result.reserve(static_cast<size_t>(list.size()));
    for (const QVariant& point : list) {
        result.push_back(PianomaniaHeldNotePitchCurvePoint::fromQVariant(point));
    }
    return result;
}
#endif

}
