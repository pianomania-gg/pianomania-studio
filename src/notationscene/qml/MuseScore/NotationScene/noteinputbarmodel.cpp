/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
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
#include "noteinputbarmodel.h"

#include <map>
#include <cmath>

#include "types/translatablestring.h"

#include "context/shortcutcontext.h"
#include "internal/notationuiactions.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/score.h"
#include "engraving/dom/sig.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/timesig.h"

using namespace mu;
using namespace mu::notation;
using namespace muse;
using namespace muse::actions;
using namespace muse::ui;
using namespace muse::uicomponents;
using mu::engraving::PianomaniaHeldNotePitchCurve;
using mu::engraving::PianomaniaHeldNotePitchCurvePoint;

static const QString TOOLBAR_NAME("noteInput");
static constexpr size_t HELD_NOTE_SETTINGS_HISTORY_LIMIT = 100;
static constexpr int HELD_NOTE_GRID_SUBDIVISIONS_PER_BEAT = 4;

namespace {
struct HeldNoteTimingMarker {
    bool writtenNoteBoundary = false;
    bool beatBoundary = false;
    int subdivision = 0;
    QString label;
};
}

static const ActionCode ADD_ACTION_CODE("add");
static const ActionCode CROSS_STAFF_BEAMING_CODE("cross-staff-beaming");
static const ActionCode TUPLET_ACTION_CODE("tuplet");

static const std::map<QString, mu::engraving::Note::PianomaniaHeldNotePulse> HELD_NOTE_PULSE_ACTIONS {
    { "held-note-pulse-quarter", mu::engraving::Note::PianomaniaHeldNotePulse::Quarter },
    { "held-note-pulse-eighth", mu::engraving::Note::PianomaniaHeldNotePulse::Eighth },
    { "held-note-pulse-sixteenth", mu::engraving::Note::PianomaniaHeldNotePulse::Sixteenth },
    { "held-note-pulse-thirty-second", mu::engraving::Note::PianomaniaHeldNotePulse::ThirtySecond },
    { "held-note-pulse-sixty-fourth", mu::engraving::Note::PianomaniaHeldNotePulse::SixtyFourth },
};

static const std::unordered_map<ActionCode, NoteInputMethod> NOTE_INPUT_METHOD_ACTIONS {
    { "note-input-by-note-name", NoteInputMethod::BY_NOTE_NAME },
    { "note-input-by-duration", NoteInputMethod::BY_DURATION },
    { "note-input-rhythm", NoteInputMethod::RHYTHM },
    { "note-input-repitch", NoteInputMethod::REPITCH },
    { "note-input-realtime-auto", NoteInputMethod::REALTIME_AUTO },
    { "note-input-realtime-manual", NoteInputMethod::REALTIME_MANUAL },
    { "note-input-timewise", NoteInputMethod::TIMEWISE },
};

NoteInputBarModel::NoteInputBarModel(QObject* parent, int heldNoteDurationTicks)
    : AbstractMenuModel(parent), m_heldNoteDurationTicks(heldNoteDurationTicks)
{
}

QVariantList NoteInputBarModel::heldNotePitchCurve() const
{
    return mu::engraving::pianomaniaHeldNotePitchCurveToQVariant(m_heldNotePitchCurve).toList();
}

int NoteInputBarModel::scoreTicksPerQuarter() const
{
    return mu::engraving::Constants::DIVISION;
}

QVariantList NoteInputBarModel::heldNoteTimingGrid() const
{
    return heldNoteTimingGridForChain(m_heldNoteSettingsChain);
}

QVariantList NoteInputBarModel::heldNoteTimingGridForChain(const std::vector<Note*>& chain)
{
    if (chain.empty() || !chain.front() || !chain.back() || !chain.front()->chord() || !chain.back()->chord()) {
        return {};
    }

    const Chord* firstChord = chain.front()->chord();
    const Chord* lastChord = chain.back()->chord();
    mu::engraving::Score* score = firstChord->score();
    Staff* staff = firstChord->staff();
    const Fraction startTick = firstChord->tick();
    const Fraction endTick = lastChord->tick() + lastChord->actualTicks();
    const Fraction totalTicks = endTick - startTick;
    if (!score || !staff || totalTicks <= Fraction(0, 1)) {
        return {};
    }

    std::map<Fraction, HeldNoteTimingMarker> markers;
    markers[startTick].writtenNoteBoundary = true;
    markers[endTick].writtenNoteBoundary = true;
    for (size_t i = 1; i < chain.size(); ++i) {
        const Note* note = chain[i];
        const Chord* chord = note ? note->chord() : nullptr;
        if (!chord || chord->score() != score || chord->tick() <= startTick || chord->tick() >= endTick) {
            return {};
        }
        markers[chord->tick()].writtenNoteBoundary = true;
    }

    for (Measure* measure = score->tick2measure(startTick); measure && measure->tick() < endTick;
         measure = measure->nextMeasure()) {
        const Fraction measureTick = measure->tick();
        const mu::engraving::TimeSig* localTimeSig = staff->timeSig(measureTick);
        const mu::engraving::TimeSigFrac timeSig(localTimeSig ? localTimeSig->sig() : measure->timesig());
        const Fraction stretch = staff->timeStretch(measureTick);
        if (!timeSig.isValid() || !stretch.isValid() || stretch.isZero()) {
            return {};
        }
        const Fraction beatTicks = Fraction::fromTicks(timeSig.beatTicks()) / stretch;
        if (beatTicks <= Fraction(0, 1)) {
            return {};
        }
        int beatIndex = 0;
        for (Fraction beatTick = measureTick; beatTick < measure->endTick(); beatTick += beatTicks, ++beatIndex) {
            for (int subdivision = 0; subdivision < HELD_NOTE_GRID_SUBDIVISIONS_PER_BEAT; ++subdivision) {
                const Fraction gridTick = beatTick + beatTicks * Fraction(subdivision, HELD_NOTE_GRID_SUBDIVISIONS_PER_BEAT);
                if (gridTick < startTick || gridTick >= endTick) {
                    continue;
                }
                HeldNoteTimingMarker& marker = markers[gridTick];
                marker.subdivision = subdivision;
                if (subdivision == 0) {
                    marker.beatBoundary = true;
                    marker.label = QString("%1.%2").arg(measure->no() + 1).arg(beatIndex + 1);
                }
            }
        }
    }

    QVariantList result;
    result.reserve(static_cast<qsizetype>(markers.size()));
    for (const auto& [tick, marker] : markers) {
        result.append(QVariantMap {
            { "scoreTick", (tick - startTick).ticks() },
            { "writtenNoteBoundary", marker.writtenNoteBoundary },
            { "beatBoundary", marker.beatBoundary },
            { "subdivision", marker.subdivision },
            { "label", marker.label },
        });
    }
    return result;
}

QString NoteInputBarModel::heldNotePositionLabel(int scoreTick) const
{
    return heldNotePositionLabelForChain(m_heldNoteSettingsChain, scoreTick);
}

QString NoteInputBarModel::heldNotePositionLabelForChain(const std::vector<Note*>& chain, int scoreTick)
{
    if (chain.empty() || !chain.front() || !chain.front()->chord() || scoreTick < 0
        || scoreTick > heldNoteDurationTicksForChain(chain)) {
        return {};
    }
    const Chord* firstChord = chain.front()->chord();
    mu::engraving::Score* score = firstChord->score();
    Staff* staff = firstChord->staff();
    const Fraction absoluteTick = firstChord->tick() + Fraction::fromTicks(scoreTick);
    Measure* measure = score ? score->tick2measure(absoluteTick) : nullptr;
    if (!measure || !staff) {
        return {};
    }
    const mu::engraving::TimeSig* localTimeSig = staff->timeSig(measure->tick());
    const mu::engraving::TimeSigFrac timeSig(localTimeSig ? localTimeSig->sig() : measure->timesig());
    const Fraction stretch = staff->timeStretch(measure->tick());
    if (!timeSig.isValid() || !stretch.isValid() || stretch.isZero()) {
        return {};
    }
    const Fraction beatTicks = Fraction::fromTicks(timeSig.beatTicks()) / stretch;
    if (beatTicks <= Fraction(0, 1)) {
        return {};
    }
    const Fraction relativeMeasureTick = absoluteTick - measure->tick();
    int beatIndex = 0;
    Fraction beatStart;
    while (beatStart + beatTicks <= relativeMeasureTick) {
        beatStart += beatTicks;
        ++beatIndex;
    }
    const int offsetTicks = (relativeMeasureTick - beatStart).ticks();
    const QString beatLabel = QString("%1.%2").arg(measure->no() + 1).arg(beatIndex + 1);
    return offsetTicks == 0 ? beatLabel : qtrc("notation", "%1 + %2 ticks").arg(beatLabel).arg(offsetTicks);
}

int NoteInputBarModel::heldNoteDurationTicksForChain(const std::vector<Note*>& chain)
{
    if (chain.empty() || !chain.front() || !chain.back() || !chain.front()->chord() || !chain.back()->chord()) {
        return 0;
    }
    return (chain.back()->chord()->tick() + chain.back()->chord()->actualTicks()
            - chain.front()->chord()->tick()).ticks();
}

bool NoteInputBarModel::heldNoteSettingsValid() const
{
    return !m_heldNotePitchBendEnabled
           || Note::isValidPianomaniaHeldNotePitchCurve(m_heldNotePitchCurve, m_heldNoteDurationTicks);
}

NoteInputBarModel::HeldNoteSettingsState NoteInputBarModel::heldNoteSettingsState() const
{
    return { m_heldNotePitchCurve, m_heldNotePitchBendEnabled, m_heldNotePulse, m_heldNotePulseTriplet,
             m_heldNotePulseMixed, m_heldNotePulseTripletMixed, m_heldNotePulseChanged, m_heldNotePulseTripletChanged };
}

void NoteInputBarModel::restoreHeldNoteSettingsState(const HeldNoteSettingsState& state)
{
    m_heldNotePitchCurve = state.pitchCurve;
    m_heldNotePitchBendEnabled = state.pitchBendEnabled;
    m_heldNotePulse = state.pulse;
    m_heldNotePulseTriplet = state.pulseTriplet;
    m_heldNotePulseMixed = state.pulseMixed;
    m_heldNotePulseTripletMixed = state.pulseTripletMixed;
    m_heldNotePulseChanged = state.pulseChanged;
    m_heldNotePulseTripletChanged = state.pulseTripletChanged;
    emit heldNoteSettingsChanged();
}

void NoteInputBarModel::recordHeldNoteSettingsEdit()
{
    if (m_heldNoteSettingsEditStart.has_value()) {
        return;
    }
    if (m_heldNoteSettingsUndoHistory.size() == HELD_NOTE_SETTINGS_HISTORY_LIMIT) {
        m_heldNoteSettingsUndoHistory.erase(m_heldNoteSettingsUndoHistory.begin());
    }
    m_heldNoteSettingsUndoHistory.push_back(heldNoteSettingsState());
    m_heldNoteSettingsRedoHistory.clear();
}

void NoteInputBarModel::clearHeldNoteSettingsHistory()
{
    m_heldNoteSettingsUndoHistory.clear();
    m_heldNoteSettingsRedoHistory.clear();
    m_heldNoteSettingsEditStart.reset();
}

void NoteInputBarModel::beginHeldNoteSettingsEdit()
{
    if (!m_heldNoteSettingsEditStart.has_value()) {
        m_heldNoteSettingsEditStart = heldNoteSettingsState();
    }
}

void NoteInputBarModel::endHeldNoteSettingsEdit()
{
    if (!m_heldNoteSettingsEditStart.has_value()) {
        return;
    }
    const HeldNoteSettingsState start = *m_heldNoteSettingsEditStart;
    m_heldNoteSettingsEditStart.reset();
    if (start == heldNoteSettingsState()) {
        return;
    }
    if (m_heldNoteSettingsUndoHistory.size() == HELD_NOTE_SETTINGS_HISTORY_LIMIT) {
        m_heldNoteSettingsUndoHistory.erase(m_heldNoteSettingsUndoHistory.begin());
    }
    m_heldNoteSettingsUndoHistory.push_back(start);
    m_heldNoteSettingsRedoHistory.clear();
    emit heldNoteSettingsChanged();
}

void NoteInputBarModel::undoHeldNoteSettingsEdit()
{
    endHeldNoteSettingsEdit();
    if (m_heldNoteSettingsUndoHistory.empty()) {
        return;
    }
    m_heldNoteSettingsRedoHistory.push_back(heldNoteSettingsState());
    const HeldNoteSettingsState state = m_heldNoteSettingsUndoHistory.back();
    m_heldNoteSettingsUndoHistory.pop_back();
    restoreHeldNoteSettingsState(state);
}

void NoteInputBarModel::redoHeldNoteSettingsEdit()
{
    endHeldNoteSettingsEdit();
    if (m_heldNoteSettingsRedoHistory.empty()) {
        return;
    }
    m_heldNoteSettingsUndoHistory.push_back(heldNoteSettingsState());
    const HeldNoteSettingsState state = m_heldNoteSettingsRedoHistory.back();
    m_heldNoteSettingsRedoHistory.pop_back();
    restoreHeldNoteSettingsState(state);
}

void NoteInputBarModel::setHeldNotePitchBendEnabled(bool enabled)
{
    if (m_heldNotePitchBendEnabled == enabled) {
        return;
    }
    recordHeldNoteSettingsEdit();
    m_heldNotePitchBendEnabled = enabled;
    if (enabled && m_heldNotePitchCurve.empty() && m_heldNoteDurationTicks > 0) {
        m_heldNotePitchCurve = { PianomaniaHeldNotePitchCurvePoint(0, 0),
                                 PianomaniaHeldNotePitchCurvePoint(m_heldNoteDurationTicks, 0) };
    }
    emit heldNoteSettingsChanged();
}

void NoteInputBarModel::setHeldNotePulse(int pulse)
{
    const auto value = static_cast<Note::PianomaniaHeldNotePulse>(pulse);
    if (pulse != 0 && value != Note::PianomaniaHeldNotePulse::Quarter && value != Note::PianomaniaHeldNotePulse::Eighth
        && value != Note::PianomaniaHeldNotePulse::Sixteenth && value != Note::PianomaniaHeldNotePulse::ThirtySecond
        && value != Note::PianomaniaHeldNotePulse::SixtyFourth) {
        return;
    }
    if (!m_heldNotePulseMixed && m_heldNotePulse == pulse) {
        return;
    }
    recordHeldNoteSettingsEdit();
    m_heldNotePulse = pulse;
    m_heldNotePulseMixed = false;
    m_heldNotePulseChanged = true;
    if (pulse == 0) {
        m_heldNotePulseTriplet = false;
        m_heldNotePulseTripletMixed = false;
        m_heldNotePulseTripletChanged = true;
    }
    emit heldNoteSettingsChanged();
}

void NoteInputBarModel::setHeldNotePulseTriplet(bool triplet)
{
    triplet = m_heldNotePulse != 0 && triplet;
    if (!m_heldNotePulseTripletMixed && m_heldNotePulseTriplet == triplet) {
        return;
    }
    recordHeldNoteSettingsEdit();
    m_heldNotePulseTriplet = triplet;
    m_heldNotePulseTripletMixed = false;
    m_heldNotePulseTripletChanged = true;
    emit heldNoteSettingsChanged();
}

void NoteInputBarModel::setHeldNotePitchCurvePoint(int index, int time, int pitch)
{
    if (index < 0 || static_cast<size_t>(index) >= m_heldNotePitchCurve.size() || pitch < -2400 || pitch > 2400) {
        return;
    }
    if (index == 0) {
        time = 0;
        pitch = 0;
    } else if (static_cast<size_t>(index) == m_heldNotePitchCurve.size() - 1) {
        time = m_heldNoteDurationTicks;
    } else if (time <= m_heldNotePitchCurve[index - 1].scoreTick || time >= m_heldNotePitchCurve[index + 1].scoreTick) {
        return;
    }
    if (m_heldNotePitchCurve[index].scoreTick == time && m_heldNotePitchCurve[index].pitchCents == pitch) {
        return;
    }
    recordHeldNoteSettingsEdit();
    m_heldNotePitchCurve[index].scoreTick = time;
    m_heldNotePitchCurve[index].pitchCents = pitch;
    emit heldNoteSettingsChanged();
}

void NoteInputBarModel::setHeldNotePitchCurvePointSlope(int index, int slope)
{
    if (index < 0 || static_cast<size_t>(index) >= m_heldNotePitchCurve.size()
        || std::abs(slope) > PianomaniaHeldNotePitchCurvePoint::MAX_SLOPE_CENTS_PER_QUARTER) {
        return;
    }
    if (m_heldNotePitchCurve[index].slopeCentsPerQuarter == slope) {
        return;
    }
    recordHeldNoteSettingsEdit();
    m_heldNotePitchCurve[index].slopeCentsPerQuarter = slope;
    emit heldNoteSettingsChanged();
}

void NoteInputBarModel::addHeldNotePitchCurvePoint(int time, int pitch)
{
    if (m_heldNotePitchCurve.size() >= 32 || time <= 0
        || time >= m_heldNoteDurationTicks || pitch < -2400 || pitch > 2400) {
        return;
    }
    const auto position = std::lower_bound(m_heldNotePitchCurve.begin(), m_heldNotePitchCurve.end(), time,
                                           [](const PianomaniaHeldNotePitchCurvePoint& point, int value) {
        return point.scoreTick < value;
    });
    if (position != m_heldNotePitchCurve.end() && position->scoreTick == time) {
        return;
    }
    recordHeldNoteSettingsEdit();
    m_heldNotePitchCurve.insert(position, PianomaniaHeldNotePitchCurvePoint(time, pitch, 0));
    emit heldNoteSettingsChanged();
}

void NoteInputBarModel::removeHeldNotePitchCurvePoint(int index)
{
    if (index <= 0 || static_cast<size_t>(index + 1) >= m_heldNotePitchCurve.size()) {
        return;
    }
    recordHeldNoteSettingsEdit();
    m_heldNotePitchCurve.erase(m_heldNotePitchCurve.begin() + index);
    emit heldNoteSettingsChanged();
}

bool NoteInputBarModel::applyHeldNoteSettings()
{
    if (!m_heldNoteSettingsEditable || m_heldNoteSettingsTargets.empty()
        || resolveHeldNoteSettingsTargets() != m_heldNoteSettingsTargets) {
        return false;
    }
    if (!m_heldNotePitchBendEditable && !m_heldNotePulseChanged && !m_heldNotePulseTripletChanged) {
        return true;
    }
    const PianomaniaHeldNotePitchCurve curve = m_heldNotePitchBendEnabled ? m_heldNotePitchCurve
                                                                         : PianomaniaHeldNotePitchCurve();
    if (m_heldNotePitchBendEditable && (!Note::isValidPianomaniaHeldNotePitchCurve(curve, m_heldNoteDurationTicks)
        || (m_heldNotePitchBendEnabled && curve.empty()))) {
        return false;
    }

    auto stack = undoStack();
    Note* first = m_heldNoteSettingsTargets.front();
    if (!stack || !first || !first->score()) {
        return false;
    }
    stack->prepareChanges(muse::TranslatableString("undoableAction", "Edit Held Note settings"));
    if (m_heldNotePitchBendEditable || m_heldNotePulseChanged) {
        first->score()->setPianomaniaHeldNotePulse(m_heldNoteSettingsTargets, m_heldNotePulse);
    }
    if (m_heldNotePitchBendEditable || m_heldNotePulseTripletChanged) {
        first->score()->setPianomaniaHeldNotePulseTriplet(m_heldNoteSettingsTargets, m_heldNotePulseTriplet);
    }
    const bool applied = !m_heldNotePitchBendEditable
                         || first->score()->setPianomaniaHeldNotePitchCurve(m_heldNoteSettingsTargets, curve);
    if (applied) {
        stack->commitChanges();
        if (m_notation) {
            m_notation->notationChanged().notify();
        }
    } else {
        stack->rollbackChanges();
    }
    return applied;
}

void NoteInputBarModel::cancelHeldNoteSettings()
{
    m_heldNoteSettingsChain.clear();
    m_heldNoteSettingsTargets.clear();
    clearHeldNoteSettingsHistory();
}

QVariant NoteInputBarModel::data(const QModelIndex& index, int role) const
{
    int row = index.row();
    if (!isIndexValid(row)) {
        return QVariant();
    }

    const MenuItem* item = items().at(row);
    switch (role) {
    case OrderRole: return row;
    case SectionRole: return item->section();
    }

    return AbstractMenuModel::data(index, role);
}

QHash<int, QByteArray> NoteInputBarModel::roleNames() const
{
    QHash<int, QByteArray> roles = AbstractMenuModel::roleNames();
    roles[OrderRole] = "order";
    roles[SectionRole] = "section";

    return roles;
}

void NoteInputBarModel::classBegin()
{
    init();
}

void NoteInputBarModel::init()
{
    subscribeOnChanges();

    uiConfiguration()->toolConfigChanged(TOOLBAR_NAME).onNotify(this, [this]() {
        load();
    });

    context()->currentNotationChanged().onNotify(this, [this]() {
        setNotation(context()->currentNotation());
    });

    playbackController()->isPlayingChanged().onNotify(this, [this]() {
        updateState();
    });

    setNotation(context()->currentNotation());
    load();
}

void NoteInputBarModel::load()
{
    MenuItemList items;

    ToolConfig noteInputConfig = uiConfiguration()->toolConfig(TOOLBAR_NAME, NotationUiActions::defaultNoteInputBarConfig());

    int section = 0;
    for (const ToolConfig::Item& citem : noteInputConfig.items) {
        if (!citem.show) {
            continue;
        }

        if (citem.action.empty()) {
            section++;
            continue;
        }
        if (HELD_NOTE_PULSE_ACTIONS.find(QString::fromStdString(citem.action)) != HELD_NOTE_PULSE_ACTIONS.cend()
            || citem.action == ActionCode("held-note-pulse-triplet")) {
            continue;
        }

        MenuItemList subitems;
        if (citem.action == CROSS_STAFF_BEAMING_CODE) {
            subitems = makeCrossStaffBeamingItems();
        } else if (citem.action == TUPLET_ACTION_CODE) {
            subitems = makeTupletItems();
        }

        MenuItem* item = makeActionItem(uiActionsRegister()->action(citem.action), QString::number(section), subitems);
        items << item;
    }

    items << makeAddItem(QString::number(++section));
    setItems(items);
}

bool NoteInputBarModel::isInputAllowed() const
{
    return m_notation && m_notation->masterNotation()->hasParts();
}

void NoteInputBarModel::setNotation(const INotationPtr& notation)
{
    if (m_notation == notation) {
        return;
    }

    if (m_notation) {
        noteInput()->stateChanged().disconnect(this);
        interaction()->selectionChanged().disconnect(this);
        undoStack()->stackChanged().disconnect(this);
        m_notation->masterNotation()->hasPartsChanged().disconnect(this);
    }

    m_notation = notation;
    m_heldNoteSettingsChain.clear();
    m_heldNoteSettingsEditable = false;

    if (notation) {
        noteInput()->stateChanged().onNotify(this, [this]() {
            updateState();
        });

        interaction()->selectionChanged().onNotify(this, [this]() {
            updateState();
        });

        undoStack()->stackChanged().onNotify(this, [this]() {
            updateState();
        });

        // FIXME: only un-/resubscribe when master notation changes
        notation->masterNotation()->hasPartsChanged().onNotify(this, [this]() {
            emit isInputAllowedChanged();
        });
    }

    updateState();

    emit isInputAllowedChanged();
}

void NoteInputBarModel::updateItemStateChecked(MenuItem& item, bool checked)
{
    UiActionState state = item.state();
    state.checked = checked;
    item.setState(state);
}

void NoteInputBarModel::updateItemsStateChecked(const ActionCode& actionCode, bool checked)
{
    // Multiple items can have this action code: one in the toolbar itself
    // and another in the menu of the "Add" toolbutton.
    MenuItemList items = findItems(actionCode);
    for (MenuItem* item : items) {
        updateItemStateChecked(*item, checked);
    }
}

void NoteInputBarModel::updateState()
{
    for (int i = 0; i < rowCount(); ++i) {
        MenuItem& item = this->item(i);
        UiActionState state = item.state();
        state.checked = false;
        item.setState(state);
    }

    if (isInputAllowed()) {
        updateNoteInputState();
    }
}

void NoteInputBarModel::updateNoteInputState()
{
    updateNoteInputModeState();
    updateNoteDotState();
    updateNoteDurationState();
    updateNoteAccidentalState();
    updateTieState();
    updateLvState();
    updateSlurState();
    updateVoicesState();
    updateArticulationsState();
    updateRestState();
    updateAddState();
    updateHandState();
    updatePianomaniaHeldNoteState();
}

void NoteInputBarModel::updateNoteInputModeState()
{
    bool isNoteInput = isNoteInputMode();
    NoteInputMethod currInputMethod = noteInputState().noteEntryMethod();

    for (int i = 0; i < rowCount(); ++i) {
        MenuItem& item = this->item(i);

        auto methodIt = NOTE_INPUT_METHOD_ACTIONS.find(item.action().code);
        if (methodIt != NOTE_INPUT_METHOD_ACTIONS.end()) {
            updateItemStateChecked(item, isNoteInput && methodIt->second == currInputMethod);
        }
    }
}

void NoteInputBarModel::updateNoteDotState()
{
    static const ActionCodeList dotActions = {
        "pad-dot",
        "pad-dot2",
        "pad-dot3",
        "pad-dot4"
    };

    int durationDots = noteInputState().duration().dots();

    for (const ActionCode& actionCode: dotActions) {
        updateItemsStateChecked(actionCode, durationDots == NotationUiActions::actionDotCount(actionCode));
    }
}

void NoteInputBarModel::updateNoteDurationState()
{
    static const ActionCodeList noteActions = {
        "note-longa",
        "note-breve",
        "pad-note-1",
        "pad-note-2",
        "pad-note-4",
        "pad-note-8",
        "pad-note-16",
        "pad-note-32",
        "pad-note-64",
        "pad-note-128",
        "pad-note-256",
        "pad-note-512",
        "pad-note-1024"
    };

    DurationType durationType = resolveCurrentDurationType();
    for (const ActionCode& actionCode: noteActions) {
        updateItemsStateChecked(actionCode, durationType == NotationUiActions::actionDurationType(actionCode));
    }
}

void NoteInputBarModel::updateNoteAccidentalState()
{
    static const ActionCodeList accidentalActions = {
        "flat2",
        "flat",
        "nat",
        "sharp",
        "sharp2"
    };

    AccidentalType accidentalType = noteInputState().accidentalType();

    for (const ActionCode& actionCode: accidentalActions) {
        updateItemsStateChecked(actionCode, accidentalType == NotationUiActions::actionAccidentalType(actionCode));
    }
}

void NoteInputBarModel::updateTieState()
{
    if (!selection()) {
        return;
    }

    std::vector<Note*> tiedNotes = selection()->notes(NoteFilter::WithTie);

    bool checked = !tiedNotes.empty();
    for (const Note* note: tiedNotes) {
        if (!note->tieFor()) {
            checked = false;
            break;
        }
        if (note->laissezVib()) {
            checked = false;
            break;
        }
    }

    updateItemsStateChecked(codeFromQString("tie"), checked); // todo
}

void NoteInputBarModel::updateLvState()
{
    if (!selection()) {
        return;
    }

    std::vector<Note*> tiedNotes = selection()->notes(NoteFilter::WithTie);

    bool checked = !tiedNotes.empty();
    for (const Note* note: tiedNotes) {
        if (!note->laissezVib()) {
            checked = false;
            break;
        }
    }

    updateItemsStateChecked(codeFromQString("lv"), checked);
}

void NoteInputBarModel::updateSlurState()
{
    bool checked = m_notation ? m_notation->elements()->msScore()->inputState().slur() != nullptr : false;
    updateItemsStateChecked(codeFromQString("add-slur"), checked);
}

void NoteInputBarModel::updateVoicesState()
{
    static const ActionCodeList voiceActions {
        "voice-1",
        "voice-2",
        "voice-3",
        "voice-4"
    };

    int currentVoice = resolveCurrentVoiceIndex();

    for (const ActionCode& actionCode: voiceActions) {
        updateItemsStateChecked(actionCode, currentVoice == NotationUiActions::actionVoice(actionCode));
    }
}

void NoteInputBarModel::updateArticulationsState()
{
    static const ActionCodeList articulationActions {
        "add-marcato",
        "add-sforzato",
        "add-tenuto",
        "add-staccato"
    };

    std::set<SymbolId> currentArticulations = resolveCurrentArticulations();

    auto isArticulationSelected = [&currentArticulations](SymbolId articulationSymbolId) {
        return std::find(currentArticulations.begin(), currentArticulations.end(),
                         articulationSymbolId) != currentArticulations.end();
    };

    for (const ActionCode& actionCode: articulationActions) {
        updateItemsStateChecked(actionCode, isArticulationSelected(NotationUiActions::actionArticulationSymbolId(actionCode)));
    }
}

void NoteInputBarModel::updateRestState()
{
    updateItemsStateChecked(ActionCode("pad-rest"), resolveRestSelected());
}

void NoteInputBarModel::updateAddState()
{
    findItem(ADD_ACTION_CODE).setSubitems(makeAddItems());
}

void NoteInputBarModel::updateHandState()
{
    updateItemStateChecked(findItem(ActionCode("hand-visuals")), mu::engraving::MScore::showPianomaniaHands);

    if (!selection() || selection()->isNone()) {
        return;
    }

    bool allLeft = true;
    bool allRight = true;
    bool hasHandTargets = false;

    const auto notes = selection()->notes();
    for (const auto& note : notes) {
        if (!note) {
            continue;
        }
        hasHandTargets = true;
        int handValue = note->getProperty(mu::engraving::Pid::PIANOMANIA_HAND).toInt();
        if (handValue != static_cast<int>(mu::engraving::Note::PianomaniaHand::Left)) {
            allLeft = false;
        }
        if (handValue != static_cast<int>(mu::engraving::Note::PianomaniaHand::Right)) {
            allRight = false;
        }
    }

    const std::vector<EngravingItem*>& selectedElements = selection()->elements();
    for (const EngravingItem* element : selectedElements) {
        if (!element || !element->isRest()) {
            continue;
        }

        hasHandTargets = true;
        int handValue = element->getProperty(mu::engraving::Pid::PIANOMANIA_HAND).toInt();
        if (handValue != static_cast<int>(mu::engraving::Note::PianomaniaHand::Left)) {
            allLeft = false;
        }
        if (handValue != static_cast<int>(mu::engraving::Note::PianomaniaHand::Right)) {
            allRight = false;
        }
    }

    if (!hasHandTargets) {
        return;
    }

    updateItemStateChecked(findItem(ActionCode("left-hand")), allLeft);
    updateItemStateChecked(findItem(ActionCode("right-hand")), allRight);
}

void NoteInputBarModel::updatePianomaniaHeldNoteState()
{
    bool allHeld = false;

    if (selection()) {
        const auto notes = selection()->notes();
        std::set<const Note*> handledNotes;
        bool hasChain = false;
        allHeld = true;

        for (const Note* selectedNote : notes) {
            if (!selectedNote || handledNotes.find(selectedNote) != handledNotes.cend()) {
                continue;
            }
            const std::vector<Note*> tieChain = selectedNote->tiedNotes();
            hasChain = hasChain || !tieChain.empty();
            for (const Note* note : tieChain) {
                if (!note) {
                    continue;
                }
                handledNotes.insert(note);
                allHeld = allHeld && note->pianomaniaHeldNote();
            }
        }

        allHeld = hasChain && allHeld;
    }

    updateItemStateChecked(findItem(ActionCode("held-note")), allHeld);
    MenuItem& settingsItem = findItem(ActionCode("held-note-settings"));
    UiActionState settingsState = settingsItem.state();
    settingsState.enabled = selection() && !selection()->isNone();
    settingsItem.setState(settingsState);
}

int NoteInputBarModel::resolveCurrentVoiceIndex() const
{
    constexpr int INVALID_VOICE = -1;

    if (!noteInput() || !selection()) {
        return INVALID_VOICE;
    }

    if (isNoteInputMode()) {
        return static_cast<int>(noteInputState().voice());
    }

    if (selection()->isNone()) {
        return INVALID_VOICE;
    }

    const std::vector<EngravingItem*>& selectedElements = selection()->elements();
    if (selectedElements.empty()) {
        return INVALID_VOICE;
    }

    int voice = INVALID_VOICE;
    for (const EngravingItem* element : selectedElements) {
        if (element->hasVoiceAssignmentProperties()) {
            VoiceAssignment voiceAssignment = element->getProperty(Pid::VOICE_ASSIGNMENT).value<VoiceAssignment>();
            if (voiceAssignment == VoiceAssignment::ALL_VOICE_IN_INSTRUMENT || voiceAssignment == VoiceAssignment::ALL_VOICE_IN_STAFF) {
                return INVALID_VOICE;
            }
        }
        int elementVoice = static_cast<int>(element->voice());
        if (elementVoice != voice && voice != INVALID_VOICE) {
            return INVALID_VOICE;
        }

        voice = static_cast<int>(element->voice());
    }

    return voice;
}

std::set<SymbolId> NoteInputBarModel::resolveCurrentArticulations() const
{
    if (!noteInput() || !selection()) {
        return {};
    }

    if (isNoteInputMode()) {
        return mu::engraving::splitArticulations(noteInputState().articulationIds());
    }

    if (selection()->isNone()) {
        return {};
    }

    auto chordArticulations = [](const Chord* chord) {
        std::set<SymbolId> result;
        for (Articulation* articulation: chord->articulations()) {
            result.insert(articulation->symId());
        }

        result = mu::engraving::flipArticulations(result, mu::engraving::PlacementV::ABOVE);
        return mu::engraving::splitArticulations(result);
    };

    std::set<SymbolId> result;
    bool isFirstNote = true;
    for (const EngravingItem* element: selection()->elements()) {
        if (!element->isNote()) {
            continue;
        }

        const Note* note = toNote(element);
        if (isFirstNote) {
            result = chordArticulations(note->chord());
            isFirstNote = false;
        } else {
            std::set<SymbolId> currentNoteArticulations = chordArticulations(note->chord());
            for (auto it = result.begin(); it != result.end();) {
                if (std::find(currentNoteArticulations.begin(), currentNoteArticulations.end(),
                              *it) == currentNoteArticulations.end()) {
                    it = result.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    return result;
}

bool NoteInputBarModel::resolveRestSelected() const
{
    if (!noteInput() || !selection()) {
        return false;
    }

    if (isNoteInputMode()) {
        return noteInputState().rest();
    }

    if (selection()->isNone() || selection()->isRange()) {
        return false;
    }

    for (const EngravingItem* element: selection()->elements()) {
        if (!element->isRest()) {
            return false;
        }
    }

    return true;
}

DurationType NoteInputBarModel::resolveCurrentDurationType() const
{
    constexpr DurationType INVALID_DURATION_TYPE = DurationType::V_INVALID;

    if (!noteInput() || !selection()) {
        return INVALID_DURATION_TYPE;
    }

    if (isNoteInputMode()) {
        return noteInputState().duration().type();
    }

    if (selection()->isNone() || selection()->isRange()) {
        return INVALID_DURATION_TYPE;
    }

    const std::vector<EngravingItem*>& selectedElements = selection()->elements();
    if (selectedElements.empty()) {
        return INVALID_DURATION_TYPE;
    }

    DurationType result = INVALID_DURATION_TYPE;
    bool isFirstElement = true;
    for (const EngravingItem* element: selectedElements) {
        const ChordRest* chordRest = elementToChordRest(element);
        if (!chordRest) {
            continue;
        }

        if (isFirstElement) {
            result = chordRest->durationType().type();
            isFirstElement = false;
        } else if (result != chordRest->durationType().type()) {
            return INVALID_DURATION_TYPE;
        }
    }

    return result;
}

MenuItem* NoteInputBarModel::makeActionItem(const UiAction& action, const QString& section,
                                            const muse::uicomponents::MenuItemList& subitems)
{
    MenuItem* item = new MenuItem(action, this);
    item->setSection(section);
    item->setSubitems(subitems);
    return item;
}

MenuItem* NoteInputBarModel::makeAddItem(const QString& section)
{
    static const UiAction addAction(ADD_ACTION_CODE, UiCtxAny, mu::context::CTX_ANY,
                                    TranslatableString("global", "Add"),
                                    IconCode::Code::PLUS);

    return makeActionItem(addAction, section, makeAddItems());
}

MenuItemList NoteInputBarModel::makeCrossStaffBeamingItems()
{
    MenuItemList items {
        makeMenuItem("move-up"),
        makeMenuItem("move-down")
    };

    return items;
}

MenuItemList NoteInputBarModel::makeTupletItems()
{
    MenuItemList items {
        makeMenuItem("duplet"),
        makeMenuItem("triplet"),
        makeMenuItem("quadruplet"),
        makeMenuItem("quintuplet"),
        makeMenuItem("sextuplet"),
        makeMenuItem("septuplet"),
        makeMenuItem("octuplet"),
        makeMenuItem("nonuplet"),
        makeMenuItem("tuplet-dialog")
    };

    return items;
}

MenuItemList NoteInputBarModel::makeAddItems()
{
    MenuItemList items {
        makeMenu(TranslatableString("notation", "Notes"), makeNotesItems()),
        makeMenu(TranslatableString("notation", "Intervals"), makeIntervalsItems()),
        makeMenu(TranslatableString("notation", "Measures"), makeMeasuresItems()),
        makeMenu(TranslatableString("notation", "Frames"), makeFramesItems()),
        makeMenu(TranslatableString("notation", "Text"), makeTextItems()),
        makeMenu(TranslatableString("notation", "Lines"), makeLinesItems()),
        makeMenu(TranslatableString("notation", "Chords and fretboard diagrams"), makeChordAndFretboardDiagramsItems()),
    };

    return items;
}

MenuItemList NoteInputBarModel::makeNotesItems()
{
    MenuItemList items {
        makeMenuItem("note-c"),
        makeMenuItem("note-d"),
        makeMenuItem("note-e"),
        makeMenuItem("note-f"),
        makeMenuItem("note-g"),
        makeMenuItem("note-a"),
        makeMenuItem("note-b"),
        makeSeparator(),
        makeMenuItem("chord-c"),
        makeMenuItem("chord-d"),
        makeMenuItem("chord-e"),
        makeMenuItem("chord-f"),
        makeMenuItem("chord-g"),
        makeMenuItem("chord-a"),
        makeMenuItem("chord-b")
    };

    return items;
}

MenuItemList NoteInputBarModel::makeIntervalsItems()
{
    MenuItemList items {
        makeMenuItem("interval1"),
        makeMenuItem("interval2"),
        makeMenuItem("interval3"),
        makeMenuItem("interval4"),
        makeMenuItem("interval5"),
        makeMenuItem("interval6"),
        makeMenuItem("interval7"),
        makeMenuItem("interval8"),
        makeMenuItem("interval9"),
        makeMenuItem("interval10"),
        makeSeparator(),
        makeMenuItem("interval-2"),
        makeMenuItem("interval-3"),
        makeMenuItem("interval-4"),
        makeMenuItem("interval-5"),
        makeMenuItem("interval-6"),
        makeMenuItem("interval-7"),
        makeMenuItem("interval-8"),
        makeMenuItem("interval-9"),
        makeMenuItem("interval-10")
    };

    return items;
}

MenuItemList NoteInputBarModel::makeMeasuresItems()
{
    MenuItemList items {
        makeMenuItem("insert-measure"),
        makeMenuItem("append-measure"),
        makeSeparator(),
        makeMenuItem("insert-measures"),
        makeMenuItem("insert-measures-after-selection"),
        makeSeparator(),
        makeMenuItem("insert-measures-at-start-of-score"),
        makeMenuItem("append-measures")
    };

    return items;
}

MenuItemList NoteInputBarModel::makeFramesItems()
{
    MenuItemList items {
        makeMenuItem("insert-hbox"),
        makeMenuItem("insert-vbox"),
        makeMenuItem("insert-textframe"),
        makeMenuItem("insert-fretframe"),
        makeSeparator(),
        makeMenu(TranslatableString("notation", "Insert at end of score"), makeFramesAppendItems())
    };

    return items;
}

MenuItemList NoteInputBarModel::makeFramesAppendItems()
{
    MenuItemList items {
        makeMenuItem("append-hbox"),
        makeMenuItem("append-vbox"),
        makeMenuItem("append-textframe"),
        makeMenuItem("append-fretframe")
    };

    return items;
}

MenuItemList NoteInputBarModel::makeTextItems()
{
    MenuItemList items {
        makeMenuItem("title-text"),
        makeMenuItem("subtitle-text"),
        makeMenuItem("composer-text"),
        makeMenuItem("poet-text"),
        makeMenuItem("part-text"),
        makeSeparator(),
        makeMenuItem("system-text"),
        makeMenuItem("staff-text"),
        makeMenuItem("add-dynamic"),
        makeMenuItem("expression-text"),
        makeMenuItem("rehearsalmark-text"),
        makeMenuItem("instrument-change-text"),
        makeMenuItem("fingering-text"),
        makeSeparator(),
        makeMenuItem("sticking-text"),
        makeMenuItem("chord-text"),
        makeMenuItem("roman-numeral-text"),
        makeMenuItem("nashville-number-text"),
        makeMenuItem("lyrics"),
        makeMenuItem("figured-bass"),
        makeMenuItem("tempo")
    };

    return items;
}

MenuItemList NoteInputBarModel::makeLinesItems()
{
    MenuItemList items {
        makeMenuItem("add-slur"),
        makeMenuItem("add-hairpin"),
        makeMenuItem("add-hairpin-reverse"),
        makeMenuItem("add-8va"),
        makeMenuItem("add-8vb"),
        makeMenuItem("add-noteline")
    };

    return items;
}

MenuItemList NoteInputBarModel::makeChordAndFretboardDiagramsItems()
{
    MenuItemList items {
        makeMenuItem("chord-text"),
        makeMenuItem("add-fretboard-diagram"),
        makeSeparator(),
        makeMenuItem("insert-fretframe", TranslatableString("notation", "Fretboard diagram legend"))
    };

    return items;
}

INotationInteractionPtr NoteInputBarModel::interaction() const
{
    return m_notation ? m_notation->interaction() : nullptr;
}

INotationSelectionPtr NoteInputBarModel::selection() const
{
    return m_notation ? m_notation->interaction()->selection() : nullptr;
}

INotationUndoStackPtr NoteInputBarModel::undoStack() const
{
    return m_notation ? m_notation->undoStack() : nullptr;
}

INotationNoteInputPtr NoteInputBarModel::noteInput() const
{
    return m_notation ? m_notation->interaction()->noteInput() : nullptr;
}

bool NoteInputBarModel::isNoteInputMode() const
{
    return m_notation ? m_notation->interaction()->noteInput()->isNoteInputMode() : false;
}

const NoteInputState& NoteInputBarModel::noteInputState() const
{
    INotationNoteInputPtr input = noteInput();
    if (!input) {
        static const NoteInputState dummyState;
        return dummyState;
    }

    return input->state();
}

const ChordRest* NoteInputBarModel::elementToChordRest(const EngravingItem* element) const
{
    if (!element) {
        return nullptr;
    }
    if (element->isChordRest()) {
        return toChordRest(element);
    }
    if (element->isNote()) {
        return toNote(element)->chord();
    }
    if (element->isStem()) {
        return toStem(element)->chord();
    }
    if (element->isHook()) {
        return toHook(element)->chord();
    }
    return nullptr;
}

std::vector<Note*> NoteInputBarModel::resolveHeldNoteSettingsChain() const
{
    if (!selection()) {
        return {};
    }

    return resolveHeldNoteSettingsChain(selection()->notes(), selection()->elements());
}

std::vector<Note*> NoteInputBarModel::resolveHeldNoteSettingsTargets() const
{
    if (!selection()) {
        return {};
    }
    return resolveHeldNoteSettingsTargets(selection()->notes(), selection()->elements());
}

std::vector<Note*> NoteInputBarModel::resolveHeldNoteSettingsTargets(const std::vector<Note*>& selectedNotes,
                                                                    const std::vector<EngravingItem*>& selectedElements)
{
    std::set<Note*> chainStarts;
    for (Note* note : selectedNotes) {
        if (note && !note->tiedNotes().empty()) {
            chainStarts.insert(note->tiedNotes().front());
        }
    }
    for (EngravingItem* element : selectedElements) {
        if (element && element->isTie()) {
            Note* note = toTie(element)->startNote();
            if (note && !note->tiedNotes().empty()) {
                chainStarts.insert(note->tiedNotes().front());
            }
        }
    }
    std::vector<Note*> targets;
    targets.reserve(chainStarts.size());
    for (Note* start : chainStarts) {
        const std::vector<Note*> chain = start ? start->tiedNotes() : std::vector<Note*>();
        if (chain.empty() || std::any_of(chain.cbegin(), chain.cend(), [](const Note* note) {
            return !note || !note->pianomaniaHeldNote();
        })) {
            return {};
        }
        targets.push_back(start);
    }
    return targets;
}

std::vector<Note*> NoteInputBarModel::resolveHeldNoteSettingsChain(const std::vector<Note*>& selectedNotes,
                                                                  const std::vector<EngravingItem*>& selectedElements)
{

    std::map<Note*, std::vector<Note*> > chains;
    auto addNote = [&chains](Note* note) {
        if (!note) {
            return;
        }
        std::vector<Note*> chain = note->tiedNotes();
        if (!chain.empty()) {
            chains[chain.front()] = std::move(chain);
        }
    };
    for (Note* note : selectedNotes) {
        addNote(note);
    }
    for (EngravingItem* element : selectedElements) {
        if (element && element->isTie()) {
            addNote(toTie(element)->startNote());
        }
    }

    if (chains.size() != 1) {
        return {};
    }
    const std::vector<Note*>& chain = chains.cbegin()->second;
    if (chain.empty() || std::any_of(chain.cbegin(), chain.cend(), [](const Note* note) {
        return !note || !note->pianomaniaHeldNote();
    })) {
        return {};
    }
    return chain;
}

void NoteInputBarModel::prepareHeldNoteSettings()
{
    clearHeldNoteSettingsHistory();
    m_heldNoteSettingsEditable = false;
    m_heldNotePitchBendEditable = false;
    m_heldNoteSettingsMessage.clear();
    m_heldNoteSettingsChain.clear();
    m_heldNoteSettingsTargets.clear();
    m_heldNotePulseMixed = false;
    m_heldNotePulseTripletMixed = false;
    m_heldNotePulseChanged = false;
    m_heldNotePulseTripletChanged = false;
    m_heldNoteSettingsTargets = resolveHeldNoteSettingsTargets();
    if (m_heldNoteSettingsTargets.empty()) {
        m_heldNoteSettingsMessage = qtrc("notation", "Select a Held Note to edit.");
        emit heldNoteSettingsChanged();
        emit heldNoteSettingsRequested();
        return;
    }

    if (m_heldNoteSettingsTargets.size() > 1) {
        const Note* firstTarget = m_heldNoteSettingsTargets.front();
        m_heldNotePulse = static_cast<int>(firstTarget->pianomaniaHeldNotePulse());
        m_heldNotePulseTriplet = firstTarget->pianomaniaHeldNotePulseTriplet();
        m_heldNotePulseMixed = std::any_of(m_heldNoteSettingsTargets.cbegin() + 1, m_heldNoteSettingsTargets.cend(),
                                           [this](const Note* note) {
            return static_cast<int>(note->pianomaniaHeldNotePulse()) != m_heldNotePulse;
        });
        m_heldNotePulseTripletMixed = std::any_of(m_heldNoteSettingsTargets.cbegin() + 1, m_heldNoteSettingsTargets.cend(),
                                                  [this](const Note* note) {
            return note->pianomaniaHeldNotePulseTriplet() != m_heldNotePulseTriplet;
        });
        m_heldNotePitchBendEnabled = false;
        m_heldNotePitchCurve.clear();
        m_heldNoteDurationTicks = 0;
        m_heldNoteSettingsMessage = qtrc("notation", "Edit the pulse for %1 Held Notes. Select one Held Note to edit its pitch bend.")
                                    .arg(m_heldNoteSettingsTargets.size());
        m_heldNoteSettingsEditable = true;
        emit heldNoteSettingsChanged();
        emit heldNoteSettingsRequested();
        return;
    }

    m_heldNoteSettingsChain = m_heldNoteSettingsTargets.front()->tiedNotes();

    const Note* note = m_heldNoteSettingsChain.front();
    m_heldNoteDurationTicks = heldNoteDurationTicksForChain(m_heldNoteSettingsChain);
    m_heldNotePitchCurve = note->pianomaniaHeldNotePitchCurve();
    m_heldNotePitchBendEnabled = !m_heldNotePitchCurve.empty();
    if (m_heldNotePitchCurve.empty()) {
        m_heldNotePitchCurve = { PianomaniaHeldNotePitchCurvePoint(0, 0),
                                 PianomaniaHeldNotePitchCurvePoint(m_heldNoteDurationTicks, 0) };
    }
    m_heldNotePulse = static_cast<int>(note->pianomaniaHeldNotePulse());
    m_heldNotePulseTriplet = note->pianomaniaHeldNotePulseTriplet();
    m_heldNoteSettingsEditable = true;
    m_heldNotePitchBendEditable = true;
    emit heldNoteSettingsChanged();
    emit heldNoteSettingsRequested();
}

void NoteInputBarModel::handleMenuItem(const QString& itemId)
{
    if (itemId == "held-note") {
        auto sel = selection();
        if (sel) {
            auto notes = sel->notes();
            if (!notes.empty() && notes[0]) {
                auto undoStack = this->undoStack();
                if (undoStack) {
                    undoStack->prepareChanges(muse::TranslatableString("notation", "Toggle held note"));
                    notes[0]->score()->togglePianomaniaHeldNotes(notes);
                    undoStack->commitChanges();
                }
            }
            // Force redraw of the score
            if (m_notation) {
                m_notation->notationChanged().notify();
            }
        }
        return;
    }
    if (itemId == "held-note-settings") {
        prepareHeldNoteSettings();
        return;
    }
    auto pulseAction = HELD_NOTE_PULSE_ACTIONS.find(itemId);
    if (pulseAction != HELD_NOTE_PULSE_ACTIONS.cend()) {
        auto sel = selection();
        if (sel) {
            auto notes = sel->notes();
            if (!notes.empty() && notes[0]) {
                auto undoStack = this->undoStack();
                if (undoStack) {
                    const MenuItem& item = findItem(codeFromQString(itemId));
                    const auto pulse = item.state().checked
                        ? Note::PianomaniaHeldNotePulse::None
                        : pulseAction->second;
                    undoStack->prepareChanges(muse::TranslatableString("notation", "Set held note pulse"));
                    notes[0]->score()->setPianomaniaHeldNotePulse(notes, static_cast<int>(pulse));
                    undoStack->commitChanges();
                }
            }
            if (m_notation) {
                m_notation->notationChanged().notify();
            }
        }
        return;
    }
    if (itemId == "held-note-pulse-triplet") {
        auto sel = selection();
        if (sel) {
            auto notes = sel->notes();
            if (!notes.empty() && notes[0]) {
                auto undoStack = this->undoStack();
                if (undoStack) {
                    const bool triplet = !findItem(ActionCode("held-note-pulse-triplet")).state().checked;
                    undoStack->prepareChanges(muse::TranslatableString("notation", "Toggle held note triplet pulse"));
                    notes[0]->score()->setPianomaniaHeldNotePulseTriplet(notes, triplet);
                    undoStack->commitChanges();
                }
            }
            if (m_notation) {
                m_notation->notationChanged().notify();
            }
        }
        return;
    }
    if (itemId == "shake-note") {
        auto sel = selection();
        if (sel) {
            auto notes = sel->notes();
            if (!notes.empty() && notes[0]) {
                auto undoStack = this->undoStack();
                if (undoStack) {
                    undoStack->prepareChanges(muse::TranslatableString("notation", "Toggle bass shake note"));
                    notes[0]->score()->togglePianomaniaShakeNotes(notes);
                    undoStack->commitChanges();
                }
            }
            // Force redraw of the score
            if (m_notation) {
                m_notation->notationChanged().notify();
            }
        }
        return;
    }
    if (itemId == "left-hand" || itemId == "right-hand") {
        auto sel = selection();
        if (sel) {
            auto notes = sel->notes();
            const auto& elements = sel->elements();
            if ((!notes.empty() && notes[0]) || !elements.empty()) {
                auto undoStack = this->undoStack();
                if (undoStack) {
                    const auto targetHand = itemId == "left-hand"
                        ? mu::engraving::Note::PianomaniaHand::Left
                        : mu::engraving::Note::PianomaniaHand::Right;
                    bool allMatch = true;
                    bool hasHandTargets = false;

                    for (auto& note : notes) {
                        if (!note) {
                            continue;
                        }
                        hasHandTargets = true;
                        int handValue = note->getProperty(mu::engraving::Pid::PIANOMANIA_HAND).toInt();
                        if (handValue != static_cast<int>(targetHand)) {
                            allMatch = false;
                            break;
                        }
                    }

                    if (allMatch) {
                        for (const EngravingItem* element : elements) {
                            if (!element || !element->isRest()) {
                                continue;
                            }
                            hasHandTargets = true;
                            int handValue = element->getProperty(mu::engraving::Pid::PIANOMANIA_HAND).toInt();
                            if (handValue != static_cast<int>(targetHand)) {
                                allMatch = false;
                                break;
                            }
                        }
                    }

                    if (!hasHandTargets) {
                        return;
                    }

                    undoStack->prepareChanges(muse::TranslatableString("notation", "Toggle Pianomania hand"));
                    for (auto& note : notes) {
                        if (note) {
                            auto newValue = allMatch
                                ? mu::engraving::Note::PianomaniaHand::Undefined
                                : targetHand;
                            note->undoChangeProperty(mu::engraving::Pid::PIANOMANIA_HAND,
                                                     static_cast<int>(newValue),
                                                     mu::engraving::PropertyFlags::UNSTYLED);
                        }
                    }

                    for (EngravingItem* element : elements) {
                        if (!element || !element->isRest()) {
                            continue;
                        }
                        auto newValue = allMatch
                            ? mu::engraving::Note::PianomaniaHand::Undefined
                            : targetHand;
                        element->undoChangeProperty(mu::engraving::Pid::PIANOMANIA_HAND,
                                                    static_cast<int>(newValue),
                                                    mu::engraving::PropertyFlags::UNSTYLED);
                    }
                    undoStack->commitChanges();
                }
            }
            if (m_notation) {
                m_notation->notationChanged().notify();
            }
        }
        return;
    }
    if (itemId == "hand-visuals") {
        mu::engraving::MScore::showPianomaniaHands = !mu::engraving::MScore::showPianomaniaHands;
        updateState();
        if (m_notation) {
            m_notation->notationChanged().notify();
        }
        return;
    }
    AbstractMenuModel::handleMenuItem(itemId);
}
