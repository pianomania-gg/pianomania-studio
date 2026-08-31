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
#pragma once

#include <optional>

#include <QQmlParserStatus>
#include <qqmlintegration.h>

#include "notation/inotation.h"
#include "engraving/types/pianomaniaheldnotepitchcurve.h"
#include "uicomponents/qml/Muse/UiComponents/abstractmenumodel.h"

#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "playback/iplaybackcontroller.h"
#include "ui/iuiconfiguration.h"

namespace mu::notation {
class NoteInputBarModel : public muse::uicomponents::AbstractMenuModel, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus);
    QML_ELEMENT;

    Q_PROPERTY(bool isInputAllowed READ isInputAllowed NOTIFY isInputAllowedChanged)
    Q_PROPERTY(bool heldNoteSettingsEditable READ heldNoteSettingsEditable NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(bool heldNotePitchBendEditable READ heldNotePitchBendEditable NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(QString heldNoteSettingsMessage READ heldNoteSettingsMessage NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(QVariantList heldNotePitchCurve READ heldNotePitchCurve NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(QVariantList heldNoteTimingGrid READ heldNoteTimingGrid NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(int heldNoteDurationTicks READ heldNoteDurationTicks NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(int scoreTicksPerQuarter READ scoreTicksPerQuarter CONSTANT)
    Q_PROPERTY(bool heldNotePitchBendEnabled READ heldNotePitchBendEnabled WRITE setHeldNotePitchBendEnabled NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(bool heldNoteSettingsValid READ heldNoteSettingsValid NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(bool heldNoteSettingsCanUndo READ heldNoteSettingsCanUndo NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(bool heldNoteSettingsCanRedo READ heldNoteSettingsCanRedo NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(int heldNotePulse READ heldNotePulse WRITE setHeldNotePulse NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(bool heldNotePulseMixed READ heldNotePulseMixed NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(bool heldNotePulseTriplet READ heldNotePulseTriplet WRITE setHeldNotePulseTriplet NOTIFY heldNoteSettingsChanged)
    Q_PROPERTY(bool heldNotePulseTripletMixed READ heldNotePulseTripletMixed NOTIFY heldNoteSettingsChanged)

    muse::GlobalInject<muse::ui::IUiConfiguration> uiConfiguration;
    muse::ContextInject<context::IGlobalContext> context = { this };
    muse::ContextInject<playback::IPlaybackController> playbackController = { this };

public:
    explicit NoteInputBarModel(QObject* parent = nullptr, int heldNoteDurationTicks = 0);

    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool isInputAllowed() const;
    bool heldNoteSettingsEditable() const { return m_heldNoteSettingsEditable; }
    bool heldNotePitchBendEditable() const { return m_heldNotePitchBendEditable; }
    QString heldNoteSettingsMessage() const { return m_heldNoteSettingsMessage; }
    QVariantList heldNotePitchCurve() const;
    QVariantList heldNoteTimingGrid() const;
    Q_INVOKABLE QString heldNotePositionLabel(int scoreTick) const;
    int heldNoteDurationTicks() const { return m_heldNoteDurationTicks; }
    int scoreTicksPerQuarter() const;
    bool heldNotePitchBendEnabled() const { return m_heldNotePitchBendEnabled; }
    bool heldNoteSettingsValid() const;
    bool heldNoteSettingsCanUndo() const { return !m_heldNoteSettingsUndoHistory.empty(); }
    bool heldNoteSettingsCanRedo() const { return !m_heldNoteSettingsRedoHistory.empty(); }
    void setHeldNotePitchBendEnabled(bool enabled);
    int heldNotePulse() const { return m_heldNotePulse; }
    bool heldNotePulseMixed() const { return m_heldNotePulseMixed; }
    void setHeldNotePulse(int pulse);
    bool heldNotePulseTriplet() const { return m_heldNotePulseTriplet; }
    bool heldNotePulseTripletMixed() const { return m_heldNotePulseTripletMixed; }
    void setHeldNotePulseTriplet(bool triplet);

    Q_INVOKABLE void setHeldNotePitchCurvePoint(int index, int time, int pitch);
    Q_INVOKABLE void setHeldNotePitchCurvePointSlope(int index, int slope);
    Q_INVOKABLE void addHeldNotePitchCurvePoint(int time, int pitch);
    Q_INVOKABLE void removeHeldNotePitchCurvePoint(int index);
    Q_INVOKABLE void beginHeldNoteSettingsEdit();
    Q_INVOKABLE void endHeldNoteSettingsEdit();
    Q_INVOKABLE void undoHeldNoteSettingsEdit();
    Q_INVOKABLE void redoHeldNoteSettingsEdit();
    Q_INVOKABLE bool applyHeldNoteSettings();
    Q_INVOKABLE void cancelHeldNoteSettings();

    static std::vector<mu::engraving::Note*> resolveHeldNoteSettingsChain(
        const std::vector<mu::engraving::Note*>& selectedNotes,
        const std::vector<mu::engraving::EngravingItem*>& selectedElements);
    static std::vector<mu::engraving::Note*> resolveHeldNoteSettingsTargets(
        const std::vector<mu::engraving::Note*>& selectedNotes,
        const std::vector<mu::engraving::EngravingItem*>& selectedElements);
    static QVariantList heldNoteTimingGridForChain(const std::vector<mu::engraving::Note*>& chain);
    static QString heldNotePositionLabelForChain(const std::vector<mu::engraving::Note*>& chain, int scoreTick);
    static int heldNoteDurationTicksForChain(const std::vector<mu::engraving::Note*>& chain);

signals:
    void isInputAllowedChanged();
    void heldNoteSettingsChanged();
    void heldNoteSettingsRequested();

private:
    struct HeldNoteSettingsState {
        mu::engraving::PianomaniaHeldNotePitchCurve pitchCurve;
        bool pitchBendEnabled = false;
        int pulse = 0;
        bool pulseTriplet = false;
        bool pulseMixed = false;
        bool pulseTripletMixed = false;
        bool pulseChanged = false;
        bool pulseTripletChanged = false;

        bool operator==(const HeldNoteSettingsState& other) const
        {
            return pitchCurve == other.pitchCurve
                   && pitchBendEnabled == other.pitchBendEnabled
                   && pulse == other.pulse
                   && pulseTriplet == other.pulseTriplet
                   && pulseMixed == other.pulseMixed
                   && pulseTripletMixed == other.pulseTripletMixed
                   && pulseChanged == other.pulseChanged
                   && pulseTripletChanged == other.pulseTripletChanged;
        }
    };

    enum NoteInputRoles {
        OrderRole = AbstractMenuModel::Roles::UserRole + 1,
        SectionRole
    };

    void classBegin() override;
    void componentComplete() override {}
    void init();

    void setNotation(const INotationPtr& notation);

    void load() override;
    Q_INVOKABLE void handleMenuItem(const QString& itemId) override;

    void updateItemStateChecked(muse::uicomponents::MenuItem& item, bool checked);
    void updateItemsStateChecked(const muse::actions::ActionCode& actionCode, bool checked);

    void updateState();
    void updateNoteInputState();
    void updateNoteInputModeState();
    void updateNoteDotState();
    void updateNoteDurationState();
    void updateNoteAccidentalState();
    void updateTieState();
    void updateLvState();
    void updateSlurState();
    void updateVoicesState();
    void updateArticulationsState();
    void updateRestState();
    void updateAddState();
    void updateHandState();
    void updatePianomaniaHeldNoteState();
    void prepareHeldNoteSettings();
    HeldNoteSettingsState heldNoteSettingsState() const;
    void restoreHeldNoteSettingsState(const HeldNoteSettingsState& state);
    void recordHeldNoteSettingsEdit();
    void clearHeldNoteSettingsHistory();
    std::vector<mu::engraving::Note*> resolveHeldNoteSettingsChain() const;
    std::vector<mu::engraving::Note*> resolveHeldNoteSettingsTargets() const;

    muse::uicomponents::MenuItem* makeActionItem(const muse::ui::UiAction& action, const QString& section,
                                                 const muse::uicomponents::MenuItemList& subitems = {});
    muse::uicomponents::MenuItem* makeAddItem(const QString& section);

    muse::uicomponents::MenuItemList makeCrossStaffBeamingItems();
    muse::uicomponents::MenuItemList makeTupletItems();
    muse::uicomponents::MenuItemList makeAddItems();
    muse::uicomponents::MenuItemList makeNotesItems();
    muse::uicomponents::MenuItemList makeIntervalsItems();
    muse::uicomponents::MenuItemList makeMeasuresItems();
    muse::uicomponents::MenuItemList makeFramesItems();
    muse::uicomponents::MenuItemList makeFramesAppendItems();
    muse::uicomponents::MenuItemList makeTextItems();
    muse::uicomponents::MenuItemList makeLinesItems();
    muse::uicomponents::MenuItemList makeChordAndFretboardDiagramsItems();

    INotationNoteInputPtr noteInput() const;
    INotationInteractionPtr interaction() const;
    INotationSelectionPtr selection() const;
    INotationUndoStackPtr undoStack() const;

    int resolveCurrentVoiceIndex() const;
    std::set<SymbolId> resolveCurrentArticulations() const;
    bool resolveRestSelected() const;
    DurationType resolveCurrentDurationType() const;

    bool isNoteInputMode() const;
    const NoteInputState& noteInputState() const;

    const ChordRest* elementToChordRest(const EngravingItem* element) const;

    INotationPtr m_notation = nullptr;
    std::vector<mu::engraving::Note*> m_heldNoteSettingsChain;
    std::vector<mu::engraving::Note*> m_heldNoteSettingsTargets;
    mu::engraving::PianomaniaHeldNotePitchCurve m_heldNotePitchCurve;
    int m_heldNoteDurationTicks = 0;
    bool m_heldNoteSettingsEditable = false;
    bool m_heldNotePitchBendEditable = false;
    QString m_heldNoteSettingsMessage;
    bool m_heldNotePitchBendEnabled = false;
    int m_heldNotePulse = 0;
    bool m_heldNotePulseTriplet = false;
    bool m_heldNotePulseMixed = false;
    bool m_heldNotePulseTripletMixed = false;
    bool m_heldNotePulseChanged = false;
    bool m_heldNotePulseTripletChanged = false;
    std::vector<HeldNoteSettingsState> m_heldNoteSettingsUndoHistory;
    std::vector<HeldNoteSettingsState> m_heldNoteSettingsRedoHistory;
    std::optional<HeldNoteSettingsState> m_heldNoteSettingsEditStart;
};
}
