/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 */

#include "pmlayout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pmstyle.h"

#include "log.h"

#include "../dom/barline.h"
#include "../dom/bracketItem.h"
#include "../dom/engravingitem.h"
#include "../dom/expression.h"
#include "../dom/instrument.h"
#include "../dom/layoutbreak.h"
#include "../dom/masterscore.h"
#include "../dom/measure.h"
#include "../dom/measurebase.h"
#include "../dom/mscore.h"
#include "../dom/part.h"
#include "../dom/score.h"
#include "../editing/editsystemlocks.h"
#include "../dom/segment.h"
#include "../dom/spacer.h"
#include "../dom/spanner.h"
#include "../dom/spannermap.h"
#include "../dom/staff.h"
#include "../dom/stafftext.h"
#include "../dom/system.h"
#include "../dom/tempotext.h"
#include "../rendering/score/horizontalspacing.h"

using namespace mu::engraving;
using namespace mu::engraving::pm;

namespace {

struct MeasureInfo
{
    Measure* measure = nullptr;
    double naturalWidth = 0.0;
    double symbolWeight = 0.0;
    bool startsSection = false;
    int barsSinceSectionStart = 0;
};

std::vector<Measure*> collectMeasures(Score* score)
{
    std::vector<Measure*> measures;
    if (!score) {
        return measures;
    }

    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        measures.push_back(measure);
    }

    return measures;
}

bool isSectionBarLine(BarLineType type)
{
    return type == BarLineType::DOUBLE
           || type == BarLineType::END
           || type == BarLineType::END_REPEAT
           || type == BarLineType::END_START_REPEAT;
}

bool hasVoltaAtStart(const Score* score, const Measure* measure)
{
    if (!score || !measure) {
        return false;
    }

    const int tick = measure->tick().ticks();
    for (const auto& interval : score->spannerMap().findOverlapping(tick, tick)) {
        if (interval.value && interval.value->isVolta() && interval.start == tick) {
            return true;
        }
    }

    return false;
}

bool hasMeasureLevelSectionMarker(const Measure* measure)
{
    for (const EngravingItem* item : measure->el()) {
        if (!item || item->generated()) {
            continue;
        }

        if (item->isMarker() || item->isJump() || item->isRehearsalMark() || item->isTempoText()) {
            return true;
        }
    }

    return false;
}

bool hasStartSegmentSectionMarker(const Measure* measure)
{
    for (Segment* segment = measure->first(); segment; segment = segment->next()) {
        if (segment->rtick().isNotZero()) {
            break;
        }

        if (segment->isKeySigType() || segment->isTimeSigType()) {
            for (const EngravingItem* item : segment->elist()) {
                if (item && !item->generated()) {
                    return true;
                }
            }
        }

        for (const EngravingItem* item : segment->annotations()) {
            if (!item || item->generated()) {
                continue;
            }

            if (item->isRehearsalMark() || item->isTempoText() || item->isMarker() || item->isJump()) {
                return true;
            }
        }
    }

    return false;
}

bool startsSection(const Score* score, const std::vector<Measure*>& measures, size_t index)
{
    if (index == 0) {
        return true;
    }

    const Measure* measure = measures[index];
    const Measure* previous = measures[index - 1];

    return measure->repeatStart()
           || previous->repeatEnd()
           || isSectionBarLine(previous->endBarLineType())
           || hasVoltaAtStart(score, measure)
           || hasMeasureLevelSectionMarker(measure)
           || hasStartSegmentSectionMarker(measure);
}

double symbolWeightForItem(const EngravingItem* item)
{
    if (!item || item->generated()) {
        return 0.0;
    }

    if (item->isChord()) {
        return 2.0;
    }
    if (item->isRestFamily()) {
        return 1.2;
    }
    if (item->isClef() || item->isKeySig() || item->isTimeSig()) {
        return 2.0;
    }
    if (item->isBarLine()) {
        return 0.5;
    }
    if (item->isRehearsalMark() || item->isTempoText() || item->isDynamic()) {
        return 1.5;
    }

    return 0.7;
}

double measureSymbolWeight(const Measure* measure)
{
    if (!measure) {
        return 1.0;
    }

    double weight = measure->irregular() ? 0.5 : 1.0;
    for (Segment* segment = measure->first(); segment; segment = segment->next()) {
        bool hasChordRest = false;
        for (const EngravingItem* item : segment->elist()) {
            if (item && item->isChordRest()) {
                hasChordRest = true;
            }
            weight += symbolWeightForItem(item);
        }

        if (hasChordRest) {
            weight += 0.5;
        }

        for (const EngravingItem* item : segment->annotations()) {
            weight += symbolWeightForItem(item);
        }
    }

    return std::max(1.0, weight);
}

std::vector<MeasureInfo> collectMeasureInfo(Score* score, const std::vector<Measure*>& measures)
{
    std::vector<MeasureInfo> info;
    info.reserve(measures.size());

    int barsSinceSectionStart = 0;
    for (size_t i = 0; i < measures.size(); ++i) {
        Measure* measure = measures[i];
        const bool sectionStart = startsSection(score, measures, i);
        if (sectionStart) {
            barsSinceSectionStart = 0;
        }

        MeasureInfo measureInfo;
        measureInfo.measure = measure;
        measureInfo.naturalWidth = std::max(1.0, measure->width());
        measureInfo.symbolWeight = measureSymbolWeight(measure);
        measureInfo.startsSection = sectionStart;
        measureInfo.barsSinceSectionStart = barsSinceSectionStart;
        info.push_back(measureInfo);

        if (!measure->irregular()) {
            ++barsSinceSectionStart;
        }
    }

    return info;
}

double systemWidth(const std::vector<MeasureInfo>& measures, size_t start, size_t end, double headerAllowance)
{
    double width = start == 0 ? 0.0 : headerAllowance;
    for (size_t i = start; i < end; ++i) {
        width += measures[i].naturalWidth;
    }

    return width;
}

double systemSymbolWeight(const std::vector<MeasureInfo>& measures, size_t start, size_t end)
{
    double weight = 0.0;
    for (size_t i = start; i < end; ++i) {
        weight += measures[i].symbolWeight;
    }

    return weight;
}

double totalNaturalWidth(const std::vector<MeasureInfo>& measures)
{
    double width = 0.0;
    for (const MeasureInfo& measure : measures) {
        width += measure.naturalWidth;
    }

    return width;
}

double totalSymbolWeight(const std::vector<MeasureInfo>& measures)
{
    double weight = 0.0;
    for (const MeasureInfo& measure : measures) {
        weight += measure.symbolWeight;
    }

    return weight;
}

bool singleMeasureSystemIsForced(const std::vector<MeasureInfo>& measures, size_t start, size_t end, double targetWidth,
                                 double headerAllowance)
{
    const double width = systemWidth(measures, start, end, headerAllowance);
    if (width >= 0.90 * targetWidth) {
        return true;
    }

    const bool canShareWithPrevious = start > 0
                                      && systemWidth(measures, start - 1, end, headerAllowance) <= 1.12 * targetWidth;
    const bool canShareWithNext = end < measures.size()
                                  && systemWidth(measures, start, end + 1, headerAllowance) <= 1.12 * targetWidth;
    return !canShareWithPrevious && !canShareWithNext;
}

int phrasePenalty(const std::vector<MeasureInfo>& measures, size_t end)
{
    if (end >= measures.size() || measures[end].startsSection) {
        return 0;
    }

    const int count = measures[end].barsSinceSectionStart;
    if (count > 0 && count % 4 == 0) {
        return 0;
    }
    if (count > 0 && count % 8 == 0) {
        return 40;
    }
    if (count > 0 && count % 2 == 0) {
        return 100;
    }

    return 250;
}

bool sectionStartInsideSystem(const std::vector<MeasureInfo>& measures, size_t start, size_t end)
{
    for (size_t i = start + 1; i < end; ++i) {
        if (measures[i].startsSection) {
            return true;
        }
    }

    return false;
}

// A system count whose balanced partition leaves reduced trailing pages (e.g.
// 13 systems -> 3/3/3/2/2) costs one penalty unit per reduced page, so the
// casting-off DP prefers a page-friendly count when the line quality is close.
double pageRemainderPenalty(size_t systemCount, size_t targetSystemsPerPage)
{
    constexpr double PENALTY_PER_REDUCED_PAGE = 350.0;

    if (targetSystemsPerPage <= 1 || systemCount <= targetSystemsPerPage) {
        return 0.0;
    }

    const size_t remainder = systemCount % targetSystemsPerPage;
    if (remainder == 0) {
        return 0.0;
    }

    return PENALTY_PER_REDUCED_PAGE * double(targetSystemsPerPage - remainder);
}

struct CastOffSolution
{
    std::vector<size_t> ends;
    double lineCost = std::numeric_limits<double>::infinity();
};

struct SystemAssignmentEntry
{
    int pageIndex = -1;
    int firstMeasureTick = 0;
};

struct StretchSnapshotEntry
{
    Measure* measure = nullptr;
    double userStretch = 1.0;
    PropertyFlags flags = PropertyFlags::STYLED;
};

// Solve casting off for exactly systemCount systems.
//
// Density targets are count-relative: the primary layout rule (see
// docs/piano-score-layout-spacing-source-of-truth.md, "Spacing symbols") is
// that every system carry a comparable share of symbols, and that share only
// has meaning for a fixed system count. Judging every candidate count against
// one global density guess starves counts whose systems are legitimately
// wider or narrower than the guess.
CastOffSolution solveCastOffForCount(const std::vector<MeasureInfo>& measures, size_t systemCount, double targetWidth,
                                     double headerAllowance)
{
    const size_t n = measures.size();
    CastOffSolution solution;
    if (n == 0 || systemCount == 0 || systemCount > n) {
        return solution;
    }

    const double contentWidth = totalNaturalWidth(measures) + double(systemCount - 1) * headerAllowance;
    const double fillTarget = contentWidth / (double(systemCount) * targetWidth);
    const double symbolTarget = std::max(1.0, totalSymbolWeight(measures) / double(systemCount));
    // LINE-mode widths underestimate real page spacing, so an absolute floor
    // would prune counts whose systems render comfortably full. Sparse is a
    // defect relative to the count's own average, not to the page width.
    const double sparsityFloor = std::min(0.62, std::max(0.42, 0.72 * fillTarget));

    constexpr double INF = std::numeric_limits<double>::infinity();
    std::vector<std::vector<double> > best(n + 1, std::vector<double>(systemCount + 1, INF));
    std::vector<std::vector<size_t> > previous(n + 1, std::vector<size_t>(systemCount + 1, n + 1));
    std::vector<std::vector<int> > previousCount(n + 1, std::vector<int>(systemCount + 1, 0));
    std::vector<std::vector<int> > previousGrid(n + 1, std::vector<int>(systemCount + 1, -1));
    best[0][0] = 0.0;

    for (size_t end = 1; end <= n; ++end) {
        for (size_t start = end; start-- > 0;) {
            const double width = systemWidth(measures, start, end, headerAllowance);
            const bool finalSystem = end == n;
            const bool singleMeasure = end == start + 1;
            const double fill = width / targetWidth;
            if (!singleMeasure && width > 1.10 * targetWidth) {
                break; // width only grows as start moves left
            }
            if (!singleMeasure && !finalSystem && fill < sparsityFloor) {
                continue;
            }
            if (singleMeasure && n > 1 && !singleMeasureSystemIsForced(measures, start, end, targetWidth, headerAllowance)) {
                continue;
            }

            double baseCost = 0.0;
            const double symbolFill = systemSymbolWeight(measures, start, end) / symbolTarget;
            baseCost += 600.0 * std::pow(symbolFill - 1.0, 2.0);
            baseCost += 350.0 * std::pow(fill / fillTarget - 1.0, 2.0);
            // Systems near natural LINE-mode width will not survive real page
            // spacing (headers, courtesy elements); steer off them instead of
            // waiting for the replan veto.
            if (fill > 0.92) {
                baseCost += 3000.0 * (fill - 0.92);
            }
            if (singleMeasure && n > 1) {
                baseCost += 4000.0;
            }
            if (sectionStartInsideSystem(measures, start, end)) {
                baseCost += 80.0;
            }
            baseCost += phrasePenalty(measures, end);

            const int count = static_cast<int>(end - start);
            const int grid = measures[end - 1].barsSinceSectionStart;
            const size_t maxPreviousSystems = std::min(systemCount - 1, start);
            for (size_t previousSystems = 0; previousSystems <= maxPreviousSystems; ++previousSystems) {
                if (!std::isfinite(best[start][previousSystems])) {
                    continue;
                }

                double cost = best[start][previousSystems] + baseCost;
                if (start > 0 && previousCount[start][previousSystems] == count && previousGrid[start][previousSystems] == grid) {
                    cost += 15.0;
                }

                const size_t systems = previousSystems + 1;
                if (cost < best[end][systems]) {
                    best[end][systems] = cost;
                    previous[end][systems] = start;
                    previousCount[end][systems] = count;
                    previousGrid[end][systems] = grid;
                }
            }
        }
    }

    if (!std::isfinite(best[n][systemCount])) {
        return solution;
    }

    solution.lineCost = best[n][systemCount];
    size_t systems = systemCount;
    for (size_t end = n; end > 0 && systems > 0; end = previous[end][systems], --systems) {
        solution.ends.push_back(end);
    }
    std::reverse(solution.ends.begin(), solution.ends.end());
    return solution;
}

std::vector<size_t> computeSystemEnds(const std::vector<MeasureInfo>& measures, double targetWidth, double headerAllowance,
                                      int targetSystemsPerPage, size_t minSystemCount)
{
    const size_t n = measures.size();
    if (n == 0) {
        return {};
    }

    const size_t pageTarget = static_cast<size_t>(std::max(1, targetSystemsPerPage));

    const double totalWidth = totalNaturalWidth(measures);
    const auto fillTargetFor = [&](size_t k) {
        return (totalWidth + double(k - 1) * headerAllowance) / (double(k) * targetWidth);
    };

    // Candidate counts span comfortable average densities: planning above
    // ~0.98 of natural width just gets vetoed by the real layout, and below
    // ~0.45 the music spreads thinner than a readable system.
    size_t kMin = 1;
    while (kMin < n && fillTargetFor(kMin) > 0.98) {
        ++kMin;
    }
    kMin = std::max<size_t>(kMin, std::max<size_t>(minSystemCount, 1));
    size_t kMax = kMin;
    while (kMax + 1 <= n && fillTargetFor(kMax + 1) >= 0.45) {
        ++kMax;
    }

    // Anchor for absolute density: with purely count-relative balance terms,
    // line cost keeps improving as systems get sparser. Each system carries a
    // flat cost so the chooser prefers the denser count when consistency is
    // comparable (the docs' practical consideration: amount of music per
    // page).
    constexpr double COST_PER_SYSTEM = 120.0;

    std::vector<size_t> chosenEnds;
    double chosenScore = std::numeric_limits<double>::infinity();
    size_t chosenCount = 0;
    for (size_t k = kMin; k <= kMax || (chosenEnds.empty() && k <= n); ++k) {
        const CastOffSolution solution = solveCastOffForCount(measures, k, targetWidth, headerAllowance);
        if (solution.ends.empty()) {
            continue;
        }

        const double pagePenalty = pageRemainderPenalty(k, pageTarget);
        const double score = solution.lineCost + pagePenalty + COST_PER_SYSTEM * double(k);
        LOGI() << "PMCASTOFF count=" << k << " lineCost=" << solution.lineCost
               << " pagePenalty=" << pagePenalty << " total=" << score
               << " fillTarget=" << fillTargetFor(k);
        if (score < chosenScore) {
            chosenScore = score;
            chosenEnds = solution.ends;
            chosenCount = k;
        }
    }

    if (chosenEnds.empty()) {
        std::vector<size_t> greedy;
        size_t start = 0;
        while (start < n) {
            size_t end = start + 1;
            while (end < n) {
                const double width = systemWidth(measures, start, end + 1, headerAllowance);
                if (width > 0.88 * targetWidth) {
                    break;
                }
                ++end;
            }
            greedy.push_back(end);
            start = end;
        }
        return greedy;
    }

    {
        const double symbolTarget = std::max(1.0, totalSymbolWeight(measures) / double(chosenCount));
        size_t s = 0;
        for (size_t e : chosenEnds) {
            LOGI() << "PMCASTOFF chosen count=" << chosenCount << " sys [" << s << "," << e << ") fill="
                   << systemWidth(measures, s, e, headerAllowance) / targetWidth
                   << " symbolShare=" << systemSymbolWeight(measures, s, e) / symbolTarget;
            s = e;
        }
    }
    return chosenEnds;
}

void insertBreaks(const std::vector<size_t>& systemEnds, const std::vector<Measure*>& measures)
{
    for (size_t systemIndex = 0; systemIndex + 1 < systemEnds.size(); ++systemIndex) {
        const size_t end = systemEnds[systemIndex];
        if (end == 0 || end > measures.size()) {
            continue;
        }

        measures[end - 1]->undoSetBreak(true, LayoutBreakType::LINE);
    }
}

void removeLayoutBreaks(Score* score)
{
    std::vector<EngravingItem*> breaks;
    for (MeasureBase* mb = score->first(); mb; mb = mb->next()) {
        for (EngravingItem* item : mb->el()) {
            if (item && item->isLayoutBreak()) {
                breaks.push_back(item);
            }
        }
    }

    for (EngravingItem* item : breaks) {
        score->undoRemoveElement(item);
    }
}

bool isVisibleKeyboardStaff(const Staff* staff)
{
    if (!staff || !staff->show()) {
        return false;
    }

    const Part* part = staff->part();
    if (!part || !part->show()) {
        return false;
    }

    const Instrument* instrument = part->instrument();
    return (instrument && instrument->musicXmlId().startsWith(u"keyboard."))
           || part->instrumentId().startsWith(u"keyboard.");
}

bool runHasAnyBrace(const std::vector<Staff*>& staves, size_t runStart, size_t span)
{
    for (size_t i = runStart; i < runStart + span && i < staves.size(); ++i) {
        const Staff* staff = staves[i];
        if (!staff) {
            continue;
        }
        for (const BracketItem* bracket : staff->brackets()) {
            if (bracket && bracket->bracketType() == BracketType::BRACE) {
                return true;
            }
        }
    }

    return false;
}

std::string scoreTextTempoNotePattern()
{
    const std::vector<String> glyphs = {
        String::fromUtf8("\uECA5"),
        String::fromUtf8("\uECA3"),
        String::fromUtf8("\uECA7"),
        String::fromUtf8("\uECA2"),
        String::fromUtf8("\uECA9"),
        String::fromUtf8("\uECAB"),
        String::fromUtf8("\uECA1"),
        String::fromUtf8("\uECA0"),
        String::fromUtf8("\uECAD"),
        String::fromUtf8("\uECAF"),
        String::fromUtf8("\uECB1"),
        String::fromUtf8("\uECB3"),
        String::fromUtf8("\uECB5")
    };

    std::string pattern = "(?:";
    for (size_t i = 0; i < glyphs.size(); ++i) {
        if (i > 0) {
            pattern += "|";
        }
        pattern += glyphs[i].toStdString();
    }
    pattern = "(?:<[^>]+>\\s*)*" + pattern + ")";

    const std::string augmentationDot = String::fromUtf8("\uECB7").toStdString();
    return pattern + "(?:(?:\\s|<[^>]+>)*" + augmentationDot + "){0,2}";
}

const std::regex& pianomaniaTempoEquationRegex()
{
    static const std::string tempoNotePattern = std::string(R"((?:)")
        + R"(<sym>metNote(?:QuarterUp|HalfUp|8thUp|Whole|16thUp|32ndUp|DoubleWholeSquare|DoubleWhole|64thUp|128thUp|256thUp|512thUp|1024thUp)</sym>(?:(?:\s|<sym>space</sym>)*(?:<sym>metAugmentationDot</sym>)){0,2})"
        + "|"
        + scoreTextTempoNotePattern()
        + R"())";
    static const std::string tempoEquationPattern = tempoNotePattern
        + R"((?:\s|<[^>]+>)*=\s*(?:c\.?\s*|ca\.?\s*)?\d+(?:[.,]\d+)?(?:\s*(?:-|–|—|&ndash;|&mdash;)\s*\d+(?:[.,]\d+)?)?)";
    static const std::regex re(
        std::string(R"((?:\s*(?:[,;:]|-|&ndash;|&mdash;)?\s*)?(?:\(\s*)")
        + tempoEquationPattern
        + R"(\s*\)|\[\s*)"
        + tempoEquationPattern
        + R"(\s*\]|)"
        + tempoEquationPattern
        + R"())",
        std::regex::ECMAScript);
    return re;
}

String cleanPianomaniaTempoIndicatorRemainder(const std::string& text)
{
    std::string result = text;
    result = std::regex_replace(result, std::regex(R"(\(\s*\)|\[\s*\])"), "");
    result = std::regex_replace(result, std::regex(R"((?:\s|<sym>space</sym>|&nbsp;)+)"), " ");
    result = std::regex_replace(result, std::regex(R"(^\s+|\s+$)"), "");
    result = std::regex_replace(result, std::regex(R"(^[\(\[]\s*|\s*[\)\]]$)"), "");
    result = std::regex_replace(result, std::regex(R"((?:\s*(?:[,;:]|-|–|—|&ndash;|&mdash;)\s*)+$)"), "");
    result = std::regex_replace(result, std::regex(R"(^(\s*(?:[,;:]|-|–|—|&ndash;|&mdash;)\s*)+)"), "");
    result = std::regex_replace(result, std::regex(R"(^\s+|\s+$)"), "");
    return String::fromStdString(result);
}

bool hasPianomaniaTempoIndicatorPlainRemainder(const String& text)
{
    std::string plain = text.toStdString();
    plain = std::regex_replace(plain, std::regex(R"(<[^>]+>)"), "");
    plain = std::regex_replace(plain, std::regex(R"(&nbsp;)"), " ");
    plain = std::regex_replace(plain, std::regex(R"(^\s+|\s+$)"), "");
    return !plain.empty();
}

void normalizePianomaniaTempoIndicators(MasterScore* score)
{
    if (!score) {
        return;
    }

    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            for (EngravingItem* item : segment->annotations()) {
                if (!item || !item->isTempoText()) {
                    continue;
                }

                TempoText* tempoText = toTempoText(item);
                const std::string originalText = tempoText->xmlText().toStdString();
                const std::string strippedText = std::regex_replace(originalText, pianomaniaTempoEquationRegex(), "");
                if (strippedText == originalText) {
                    continue;
                }

                const String cleanedText = cleanPianomaniaTempoIndicatorRemainder(strippedText);
                tempoText->setFollowText(false);
                if (!hasPianomaniaTempoIndicatorPlainRemainder(cleanedText)) {
                    tempoText->setXmlText(String());
                    tempoText->setVisible(false);
                } else {
                    tempoText->setXmlText(cleanedText);
                }
            }
        }
    }
}

std::string normalizedPianomaniaPlainText(const String& text)
{
    std::string normalized;
    bool pendingSpace = false;
    for (unsigned char c : text.trimmed().toStdString()) {
        if (std::isspace(c)) {
            pendingSpace = !normalized.empty();
            continue;
        }

        if (pendingSpace) {
            normalized += ' ';
            pendingSpace = false;
        }
        normalized += static_cast<char>(std::tolower(c));
    }

    return normalized;
}

bool isPianomaniaTempoVocabulary(const std::string& text)
{
    static const std::unordered_set<std::string> tempoVocabulary = {
        "allegro",
        "allegretto",
        "andante",
        "adagio",
        "moderato",
        "vivace",
        "presto",
        "lento",
        "largo",
        "a tempo",
        "rit.",
        "ritard.",
        "rall.",
        "accel.",
        "meno mosso",
        "piu mosso",
        "tempo i",
        "tempo 1"
    };

    return tempoVocabulary.find(text) != tempoVocabulary.end();
}

bool isPianomaniaExpressionVocabulary(const std::string& text)
{
    static const std::unordered_set<std::string> expressionVocabulary = {
        "dolce",
        "espressivo",
        "espress.",
        "cantabile",
        "legato",
        "leggiero",
        "grazioso",
        "tranquillo",
        "sotto voce",
        "mezza voce",
        // "con moto" is excluded on purpose: it can function as tempo/motion
        // text, so AutoLayout only converts less ambiguous expression words.
        "dolcissimo",
        "teneramente",
        "con espressione",
        "semplice",
        "delicato",
        "dolente",
        "espressivamente",
        "cantando",
        "cantabile e dolce"
    };

    return expressionVocabulary.find(text) != expressionVocabulary.end();
}

void resetPianomaniaExpressionFontOverrides(TextBase* text)
{
    if (!text) {
        return;
    }

    text->resetProperty(Pid::FONT_FACE);
    text->resetProperty(Pid::FONT_STYLE);
    text->resetProperty(Pid::FONT_SIZE);
}

void normalizePianomaniaExpressionText(MasterScore* score)
{
    if (!score) {
        return;
    }

    for (Measure* measure = score->firstMeasure(); measure; measure = measure->nextMeasure()) {
        for (Segment* segment = measure->first(); segment; segment = segment->next()) {
            for (EngravingItem* item : segment->annotations()) {
                if (!item) {
                    continue;
                }

                if (item->isExpression()) {
                    resetPianomaniaExpressionFontOverrides(toExpression(item));
                    continue;
                }

                if (!segment->isChordRestType() || !item->isStaffText()) {
                    continue;
                }

                StaffText* staffText = toStaffText(item);
                const std::string normalizedText = normalizedPianomaniaPlainText(staffText->plainText());
                if (isPianomaniaTempoVocabulary(normalizedText) || !isPianomaniaExpressionVocabulary(normalizedText)) {
                    continue;
                }

                staffText->setProperty(Pid::TEXT_STYLE, TextStyleType::EXPRESSION);
                resetPianomaniaExpressionFontOverrides(staffText);
            }
        }
    }
}

void ensurePianomaniaGrandStaffBraces(MasterScore* score)
{
    if (!score) {
        return;
    }

    const std::vector<Staff*>& staves = score->staves();
    size_t runStart = muse::nidx;
    size_t runSpan = 0;
    for (size_t staffIndex = 0; staffIndex <= staves.size(); ++staffIndex) {
        const bool inKeyboardRun = staffIndex < staves.size() && isVisibleKeyboardStaff(staves[staffIndex]);
        if (inKeyboardRun) {
            if (runSpan == 0) {
                runStart = staffIndex;
            }
            ++runSpan;
            continue;
        }

        if (runSpan >= 2 && !runHasAnyBrace(staves, runStart, runSpan)) {
            score->undoAddBracket(staves[runStart], 0, BracketType::BRACE, runSpan);
        }

        runStart = muse::nidx;
        runSpan = 0;
    }
}

std::vector<System*> systemsWithMeasures(const MasterScore* score)
{
    std::vector<System*> systems;
    for (System* system : score->systems()) {
        if (system && system->firstMeasure()) {
            systems.push_back(system);
        }
    }

    return systems;
}

std::vector<SystemAssignmentEntry> captureSystemAssignment(const MasterScore* score)
{
    std::vector<SystemAssignmentEntry> assignment;
    const Page* currentPage = nullptr;
    int pageIndex = -1;
    for (System* system : systemsWithMeasures(score)) {
        const Page* page = system->page();
        if (page != currentPage) {
            currentPage = page;
            ++pageIndex;
        }

        assignment.push_back(SystemAssignmentEntry { pageIndex, system->firstMeasure()->tick().ticks() });
    }
    return assignment;
}

bool systemAssignmentsEqual(const std::vector<SystemAssignmentEntry>& left, const std::vector<SystemAssignmentEntry>& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].pageIndex != right[i].pageIndex || left[i].firstMeasureTick != right[i].firstMeasureTick) {
            return false;
        }
    }
    return true;
}

std::vector<Measure*> measuresInSystem(System* system)
{
    std::vector<Measure*> measures;
    if (!system) {
        return measures;
    }

    for (Measure* measure = system->firstMeasure(); measure; measure = measure->nextMeasure()) {
        if (measure->system() != system) {
            break;
        }

        measures.push_back(measure);
        if (measure == system->lastMeasure()) {
            break;
        }
    }
    return measures;
}

std::vector<double> interiorBarlineXPositions(System* system)
{
    std::vector<double> positions;
    const std::vector<Measure*> measures = measuresInSystem(system);
    if (measures.size() < 2) {
        return positions;
    }

    positions.reserve(measures.size() - 1);
    for (size_t i = 1; i < measures.size(); ++i) {
        positions.push_back(measures[i]->pagePos().x());
    }
    return positions;
}

bool interiorBarlinesCoincide(System* first, System* second, double tolerance)
{
    const std::vector<double> firstPositions = interiorBarlineXPositions(first);
    const std::vector<double> secondPositions = interiorBarlineXPositions(second);
    if (firstPositions.empty() || firstPositions.size() != secondPositions.size()) {
        return false;
    }

    for (size_t i = 0; i < firstPositions.size(); ++i) {
        if (std::abs(firstPositions[i] - secondPositions[i]) > tolerance) {
            return false;
        }
    }
    return true;
}

bool hasOverfullSystems(const MasterScore* score)
{
    if (!score) {
        return false;
    }

    const double targetWidth = score->style().styleD(Sid::pagePrintableWidth) * DPI;
    for (System* system : systemsWithMeasures(score)) {
        if (rendering::score::HorizontalSpacing::computeSpacingForFullSystem(system) > targetWidth) {
            return true;
        }
    }
    return false;
}

void restoreUserStretch(const std::vector<StretchSnapshotEntry>& snapshot)
{
    for (const StretchSnapshotEntry& entry : snapshot) {
        if (entry.measure) {
            entry.measure->setUserStretch(entry.userStretch);
            entry.measure->setPropertyFlags(Pid::USER_STRETCH, entry.flags);
        }
    }
}

void offsetSystemBarlineProfile(System* system, size_t systemIndex, std::vector<StretchSnapshotEntry>& snapshot)
{
    const std::vector<Measure*> measures = measuresInSystem(system);
    if (measures.size() < 2) {
        return;
    }

    double balancingDelta = 0.0;
    for (size_t i = 0; i < measures.size(); ++i) {
        Measure* measure = measures[i];
        snapshot.push_back(StretchSnapshotEntry {
            measure,
            measure->userStretch(),
            measure->propertyFlags(Pid::USER_STRETCH)
        });

        double delta = 0.0;
        if (i + 1 < measures.size()) {
            // Alternate around the natural spacing so coincident barlines move
            // apart without changing the system's rhythmic profile by more
            // than a few percent.
            delta = ((i + systemIndex) % 2 == 0) ? 0.04 : -0.035;
            balancingDelta -= delta;
        } else {
            delta = std::clamp(balancingDelta, -0.04, 0.04);
        }

        measure->setUserStretch(std::clamp(1.0 + delta, 0.96, 1.04));
    }
}

std::vector<StretchSnapshotEntry> applyBarlineOffsetRule(MasterScore* score)
{
    if (!score) {
        return {};
    }

    const double tolerance = std::max(1.0, score->style().spatium());
    const std::vector<System*> systems = systemsWithMeasures(score);
    std::vector<StretchSnapshotEntry> stretchSnapshot;
    std::unordered_set<System*> adjustedSystems;

    for (size_t i = 0; i + 1 < systems.size(); ++i) {
        System* first = systems[i];
        System* second = systems[i + 1];
        if (!first || !second || first->page() != second->page()) {
            continue;
        }
        if (!interiorBarlinesCoincide(first, second, tolerance) || adjustedSystems.find(second) != adjustedSystems.end()) {
            continue;
        }

        offsetSystemBarlineProfile(second, i + 1, stretchSnapshot);
        adjustedSystems.insert(second);
        LOGI() << "PMBARLINE offset system=" << (i + 1)
               << " firstTick=" << second->firstMeasure()->tick().ticks();
    }

    if (stretchSnapshot.empty()) {
        LOGI() << "PMBARLINE no coincident adjacent systems";
        return {};
    }
    LOGI() << "PMBARLINE adjustedMeasures=" << stretchSnapshot.size();
    return stretchSnapshot;
}

// The casting-off widths come from a LINE-mode layout and underestimate real
// page spacing (system headers, courtesy elements at breaks), so a planned
// system can wrap into extra, page-unfriendly systems. When the realized
// boundaries differ from the plan, every natural wrap is a proof that the
// wrapped run of measures does not fit one system: inflate that run's width
// model past the casting-off bound so the next pass must split differently.
// Returns true when the plan was realized exactly.
bool calibrateWidthModel(MasterScore* score, const std::vector<Measure*>& measures,
                         const std::vector<size_t>& plannedEnds,
                         std::vector<MeasureInfo>& measureInfo, double headerAllowance, double targetWidth)
{
    std::unordered_map<const Measure*, size_t> indexOfMeasure;
    for (size_t i = 0; i < measures.size(); ++i) {
        indexOfMeasure.insert({ measures[i], i });
    }

    const std::unordered_set<size_t> plannedEndSet(plannedEnds.begin(), plannedEnds.end());
    const std::vector<System*> systems = systemsWithMeasures(score);

    std::vector<size_t> unplannedEnds;
    for (System* system : systems) {
        auto last = indexOfMeasure.find(system->lastMeasure());
        if (last == indexOfMeasure.end()) {
            continue;
        }
        const size_t end = last->second + 1;
        if (end != measures.size() && plannedEndSet.find(end) == plannedEndSet.end()) {
            unplannedEnds.push_back(end);
        }
    }

    if (unplannedEnds.empty() && systems.size() == plannedEnds.size()) {
        return true;
    }

    // First fix the systematic bias: scale each realized system's measures by
    // real spacing / modeled width (system headers and courtesy accidentals
    // only exist once breaks are placed, so dense pieces model far too thin).
    for (System* system : systems) {
        auto first = indexOfMeasure.find(system->firstMeasure());
        auto last = indexOfMeasure.find(system->lastMeasure());
        if (first == indexOfMeasure.end() || last == indexOfMeasure.end()) {
            continue;
        }

        const size_t start = first->second;
        const size_t end = last->second + 1;
        const double modelWidth = systemWidth(measureInfo, start, end, headerAllowance);
        const double realWidth = rendering::score::HorizontalSpacing::computeSpacingForFullSystem(system);
        if (modelWidth <= 0.0 || realWidth <= modelWidth) {
            continue;
        }

        const double ratio = std::min(1.5, realWidth / modelWidth);
        for (size_t i = start; i < end; ++i) {
            measureInfo[i].naturalWidth *= ratio;
        }
    }

    // Then forbid the groupings the layout disproved outright.
    for (size_t wrapEnd : unplannedEnds) {
        // The planned system containing this wrap spans [start, plannedEnd);
        // the layout proved measures [start, wrapEnd] cannot share a system.
        size_t start = 0;
        for (size_t plannedEnd : plannedEnds) {
            if (plannedEnd >= wrapEnd) {
                break;
            }
            start = plannedEnd;
        }

        const size_t provenEnd = std::min(wrapEnd + 1, measures.size());
        double contentWidth = 0.0;
        for (size_t i = start; i < provenEnd; ++i) {
            contentWidth += measureInfo[i].naturalWidth;
        }
        if (contentWidth <= 0.0) {
            continue;
        }

        const double header = start == 0 ? 0.0 : headerAllowance;
        const double forbiddenWidth = 1.11 * targetWidth - header;
        if (contentWidth >= forbiddenWidth) {
            continue;
        }

        const double ratio = forbiddenWidth / contentWidth;
        for (size_t i = start; i < provenEnd; ++i) {
            measureInfo[i].naturalWidth *= ratio;
        }
    }

    return false;
}

// Balanced high-first partition of systemCount over pageCount pages, honoring
// per-page capacity caps by pushing overflow to later pages. Empty result
// means the caps make pageCount pages infeasible.
std::vector<size_t> balancedPageSizes(size_t systemCount, size_t pageCount, size_t targetSystemsPerPage,
                                      const std::unordered_map<size_t, size_t>& pageCaps)
{
    std::vector<size_t> sizes(pageCount, systemCount / pageCount);
    for (size_t i = 0; i < systemCount % pageCount; ++i) {
        ++sizes[i];
    }

    size_t overflow = 0;
    for (size_t i = 0; i < pageCount; ++i) {
        sizes[i] += overflow;
        overflow = 0;
        size_t cap = targetSystemsPerPage;
        auto it = pageCaps.find(i);
        if (it != pageCaps.end()) {
            cap = std::min(cap, it->second);
        }
        if (sizes[i] > cap) {
            overflow = sizes[i] - cap;
            sizes[i] = cap;
        }
    }

    if (overflow > 0) {
        return {};
    }

    return sizes;
}

void applyPagePlan(const std::vector<System*>& systems, const std::vector<size_t>& pageSizes)
{
    size_t systemIndex = 0;
    for (size_t page = 0; page < pageSizes.size(); ++page) {
        for (size_t i = 0; i < pageSizes[page]; ++i, ++systemIndex) {
            if (systemIndex + 1 >= systems.size()) {
                return; // never insert a break after the final system
            }

            Measure* lastMeasure = systems[systemIndex]->lastMeasure();
            if (!lastMeasure) {
                continue;
            }

            const bool pageFinal = i + 1 == pageSizes[page];
            if (pageFinal) {
                lastMeasure->undoSetBreak(true, LayoutBreakType::PAGE);
            } else {
                lastMeasure->undoSetBreak(false, LayoutBreakType::PAGE);
                lastMeasure->undoSetBreak(true, LayoutBreakType::LINE);
            }
        }
    }
}

// Distribute systems over pages after the systems themselves are final.
// Plans balanced page sizes (e.g. 7 systems -> 3/2/2, never 3/3/1), applies
// explicit page breaks, and verifies the result against the real layout: a
// page that cannot vertically hold its planned share teaches the planner a
// capacity cap and the plan is rebalanced around it.
void planPageBreaks(MasterScore* score, const PmAutoLayoutOptions& options)
{
    if (!score) {
        return;
    }

    const size_t target = static_cast<size_t>(std::max(1, options.targetSystemsPerPage));
    std::unordered_map<size_t, size_t> pageCaps;

    for (int pass = 0; pass < std::max(1, options.maxPagePlanPasses); ++pass) {
        const std::vector<System*> systems = systemsWithMeasures(score);
        const size_t systemCount = systems.size();
        if (systemCount == 0) {
            return;
        }

        size_t pageCount = (systemCount + target - 1) / target;
        std::vector<size_t> pageSizes = balancedPageSizes(systemCount, pageCount, target, pageCaps);
        while (pageSizes.empty() && pageCount < systemCount) {
            ++pageCount;
            pageSizes = balancedPageSizes(systemCount, pageCount, target, pageCaps);
        }
        if (pageSizes.empty()) {
            pageSizes.assign(systemCount, 1);
        }

        applyPagePlan(systems, pageSizes);
        score->doLayout();

        // Every planned page starts fresh on a real page (the previous page's
        // explicit page break guarantees it), so one layout tells us the true
        // vertical fill of every planned page at once.
        const std::vector<System*> laidOutSystems = systemsWithMeasures(score);
        std::vector<size_t> pageOfSystem(laidOutSystems.size(), 0);
        {
            const Page* currentPage = nullptr;
            size_t pageIndex = 0;
            for (size_t i = 0; i < laidOutSystems.size(); ++i) {
                const Page* page = laidOutSystems[i]->page();
                if (currentPage && page != currentPage) {
                    ++pageIndex;
                }
                currentPage = page;
                pageOfSystem[i] = pageIndex;
            }
        }

        bool anySpill = false;
        bool learned = false;
        size_t firstSystem = 0;
        for (size_t page = 0; page < pageSizes.size() && firstSystem < pageOfSystem.size(); ++page) {
            const size_t plannedSize = pageSizes[page];
            const size_t lastSystem = std::min(firstSystem + plannedSize, pageOfSystem.size());
            size_t observed = 0;
            for (size_t i = firstSystem; i < lastSystem; ++i) {
                if (pageOfSystem[i] == pageOfSystem[firstSystem]) {
                    ++observed;
                }
            }
            if (observed < plannedSize) {
                anySpill = true;
                const size_t cap = std::max<size_t>(1, observed);
                auto it = pageCaps.find(page);
                if (it == pageCaps.end() || it->second > cap) {
                    pageCaps[page] = cap;
                    learned = true;
                }
            }
            firstSystem += plannedSize;
        }

        if (!anySpill || !learned) {
            return;
        }
    }
}

bool moveBreakOneMeasureLeft(Measure* measure)
{
    if (!measure) {
        return false;
    }

    Measure* previous = measure->prevMeasure();
    if (!previous || previous->system() != measure->system()) {
        return false;
    }
    if (measure->system() && measure->system()->firstMeasure() == previous) {
        return false;
    }

    const LayoutBreakType type = measure->pageBreak() ? LayoutBreakType::PAGE : LayoutBreakType::LINE;
    measure->undoSetBreak(false, LayoutBreakType::PAGE);
    measure->undoSetBreak(false, LayoutBreakType::LINE);
    previous->undoSetBreak(true, type);
    return true;
}

void verifyOverfullSystems(MasterScore* score, int maxPasses)
{
    if (!score) {
        return;
    }

    const double targetWidth = score->style().styleD(Sid::pagePrintableWidth) * DPI;
    for (int pass = 0; pass < maxPasses; ++pass) {
        bool changed = false;
        for (System* system : score->systems()) {
            Measure* lastMeasure = system ? system->lastMeasure() : nullptr;
            if (!lastMeasure || (!lastMeasure->lineBreak() && !lastMeasure->pageBreak())) {
                continue;
            }

            const double width = rendering::score::HorizontalSpacing::computeSpacingForFullSystem(system);
            if (width > targetWidth && moveBreakOneMeasureLeft(lastMeasure)) {
                changed = true;
            }
        }

        if (!changed) {
            break;
        }

        score->doLayout();
    }
}

bool removeBreakAfter(Measure* measure)
{
    if (!measure || (!measure->lineBreak() && !measure->pageBreak())) {
        return false;
    }

    measure->undoSetBreak(false, LayoutBreakType::PAGE);
    measure->undoSetBreak(false, LayoutBreakType::LINE);
    return true;
}

bool repairSingletonSystems(MasterScore* score)
{
    if (!score) {
        return false;
    }

    bool changed = false;
    for (System* system : score->systems()) {
        if (!system || system->firstMeasure() != system->lastMeasure()) {
            continue;
        }

        Measure* measure = system->firstMeasure();
        if (!measure || (!measure->prevMeasure() && !measure->nextMeasure())) {
            continue;
        }

        if (measure->nextMeasure()) {
            changed = removeBreakAfter(measure) || changed;
        } else {
            changed = removeBreakAfter(measure->prevMeasure()) || changed;
        }
    }

    return changed;
}

}

void mu::engraving::pm::resetStructuralLayout(Score* score)
{
    if (!score) {
        return;
    }

    std::vector<EngravingItem*> itemsToRemove;
    for (MeasureBase* mb = score->first(); mb; mb = mb->next()) {
        if (mb->isMeasure()) {
            Measure* measure = toMeasure(mb);
            measure->undoResetProperty(Pid::USER_STRETCH);

            for (const MStaff* staff : measure->mstaves()) {
                if (Spacer* spacer = staff->vspacerDown()) {
                    itemsToRemove.push_back(spacer);
                }
                if (Spacer* spacer = staff->vspacerUp()) {
                    itemsToRemove.push_back(spacer);
                }
            }
        }

        for (EngravingItem* item : mb->el()) {
            if (item && item->isLayoutBreak()) {
                itemsToRemove.push_back(item);
            }
        }
    }

    for (EngravingItem* item : itemsToRemove) {
        score->undoRemoveElement(item);
    }

    EditSystemLocks::undoRemoveAllLocks(score);
}

void mu::engraving::pm::applyPianomaniaAutoLayout(MasterScore* score, const PmAutoLayoutOptions& options)
{
    if (!score) {
        return;
    }

    stripHeaderFramesAndFooters(score);
    normalizePianomaniaTempoIndicators(score);
    normalizePianomaniaExpressionText(score);
    ensurePianomaniaGrandStaffBraces(score);
    applyPianomaniaStyle(score);
    resetStructuralLayout(score);

    const LayoutMode originalMode = score->layoutMode();
    score->setLayoutMode(LayoutMode::LINE);
    score->doLayout();

    const std::vector<Measure*> measures = collectMeasures(score);
    std::vector<MeasureInfo> measureInfo = collectMeasureInfo(score, measures);

    const double targetWidth = score->style().styleD(Sid::pagePrintableWidth) * DPI;
    const double headerAllowance = score->style().styleMM(Sid::systemHeaderDistance).val()
                                   + score->style().styleMM(Sid::systemHeaderTimeSigDistance).val()
                                   + score->style().spatium();

    // Cast off, then check the plan against real page spacing: an overfull
    // planned system would otherwise overflow into an extra system and ruin
    // the page-friendly count the casting-off pass chose. On overflow, feed
    // the observed widths back and replan; when replanning the same count
    // stops making progress, reality has vetoed that count — exclude it (and
    // everything below) so the chooser falls to the next-best count.
    score->setLayoutMode(LayoutMode::PAGE);
    static constexpr int maxCastOffAttempts = 6;
    size_t minSystemCount = 0;
    size_t lastPlanned = 0;
    size_t lastExcess = std::numeric_limits<size_t>::max();
    for (int attempt = 0; attempt < maxCastOffAttempts; ++attempt) {
        const std::vector<size_t> systemEnds = computeSystemEnds(measureInfo, targetWidth, headerAllowance,
                                                                 options.targetSystemsPerPage, minSystemCount);
        if (attempt > 0) {
            removeLayoutBreaks(score);
        }
        insertBreaks(systemEnds, measures);
        score->doLayout();
        const size_t realized = systemsWithMeasures(score).size();
        if (calibrateWidthModel(score, measures, systemEnds, measureInfo, headerAllowance, targetWidth)) {
            break;
        }

        const size_t planned = systemEnds.size();
        const size_t excess = realized > planned ? realized - planned : 0;
        if (planned == lastPlanned && excess >= lastExcess) {
            minSystemCount = planned + 1;
        }
        lastPlanned = planned;
        lastExcess = excess;
    }
    verifyOverfullSystems(score, options.maxVerifyPasses);
    if (repairSingletonSystems(score)) {
        score->doLayout();
    }

    // Plan pages against a slightly shorter page. The prettify passes that run
    // after vertical page stacking (cross-staff slurs, page-stage fingering
    // nudges) only grow the skylines of the *next* layout, so a plan computed
    // at the exact page height can overflow once Prettify persists those
    // offsets. The reserve stays planning-only: the margins are restored
    // before the final layout.
    const double planningBottomReserveIn = options.pagePlanBottomReserveIn;
    const double previousOddBottomMargin = score->style().styleD(Sid::pageOddBottomMargin);
    const double previousEvenBottomMargin = score->style().styleD(Sid::pageEvenBottomMargin);
    score->style().set(Sid::pageOddBottomMargin, previousOddBottomMargin + planningBottomReserveIn);
    score->style().set(Sid::pageEvenBottomMargin, previousEvenBottomMargin + planningBottomReserveIn);
    score->doLayout();
    planPageBreaks(score, options);
    score->style().set(Sid::pageOddBottomMargin, previousOddBottomMargin);
    score->style().set(Sid::pageEvenBottomMargin, previousEvenBottomMargin);

    score->setLayoutMode(originalMode == LayoutMode::LINE ? LayoutMode::PAGE : originalMode);
    const std::vector<SystemAssignmentEntry> preBarlineAssignment = captureSystemAssignment(score);
    const std::vector<StretchSnapshotEntry> barlineStretchSnapshot = applyBarlineOffsetRule(score);
    score->setLayoutAll();
    score->doLayout();
    if (!barlineStretchSnapshot.empty()) {
        const std::vector<SystemAssignmentEntry> postBarlineAssignment = captureSystemAssignment(score);
        const bool assignmentChanged = !systemAssignmentsEqual(preBarlineAssignment, postBarlineAssignment);
        const bool overfullSystems = hasOverfullSystems(score);
        if (assignmentChanged || overfullSystems) {
            restoreUserStretch(barlineStretchSnapshot);
            score->setLayoutAll();
            score->doLayout();
            LOGI() << "PMBARLINE reverted score=" << score->name()
                   << " assignmentChanged=" << assignmentChanged
                   << " overfullSystems=" << overfullSystems;
        }
    }
}
