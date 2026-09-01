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

#ifndef MU_IMPORTEXPORT_MEIEXPORTER_H
#define MU_IMPORTEXPORT_MEIEXPORTER_H

#include <optional>
#include <utility>
#include <array>

#include "engraving/types/types.h"

#include "imeiconfiguration.h"
#include "modularity/ioc.h"

#include "meiconverter.h"

#include "thirdparty/libmei/element.h"
#include "thirdparty/libmei/shared.h"

#include "thirdparty/pugixml.hpp"

namespace mu::engraving {
class Articulation;
class Beam;
class Chord;
class ChordRest;
class Clef;
class EngravingItem;
class Fingering;
class Lyrics;
class Measure;
class Note;
class Part;
class Rest;
class Slur;
class Score;
class Staff;
class TextBase;
class TextLineBase;
class TremoloSingleChord;
class Trill;
class Tuplet;
class VBox;
} // namespace mu::engraving

namespace mu::iex::mei {
class UIDRegister;

enum layerElementCounter {
  ACCID_L = 0,
  BEAM_L,
  HARM_L,
  GRACEGRP_L,
  SYL_L,
  VERSE_L,
  UNSPECIFIED_L,
};

/**
 * Class of exporting Music Encoding Initiative (MEI) files
 */

class MeiExporter {
public:
  INJECT_STATIC(mu::iex::mei::IMeiConfiguration, configuration)

  struct PianomaniaNoteRecord {
    const engraving::Note *note = nullptr;
    std::string scoreEventId;
    std::string chordId;
    std::string measureId;
    std::string beat;
    int idx = 0;
  };

public:
  MeiExporter(engraving::Score *s) { m_score = s; }
  bool write(std::string &meiData);
  void setRequirePrecomputedPianomaniaIndices(bool value) {
    m_requirePrecomputedPianomaniaIndices = value;
  }
  void setIncludeExportDate(bool value) { m_includeExportDate = value; }
  const std::vector<PianomaniaNoteRecord> &pianomaniaNoteRecords() const {
    return m_pianomaniaNoteRecords;
  }

private:
  bool writeHeader();
  bool writeScore();
  bool writeScoreDef();
  bool writePgHead(const engraving::VBox *vBox);
  bool writeLines(pugi::xml_node node, const muse::StringList &lines);
  bool writeLinesWithSMuFL(pugi::xml_node node, const muse::StringList &lines);
  bool writeScoreDefChange();
  bool writeStaffGrpStart(const engraving::Staff *staff, std::vector<int> &ends,
                          const engraving::Part *part);
  bool writeStaffGrpEnd(const engraving::Staff *staff, std::vector<int> &ends);
  bool writeStaffDef(const engraving::Staff *staff,
                     const engraving::Measure *measure,
                     const engraving::Part *part, bool isPart);
  bool writeLabel(pugi::xml_node node, const engraving::Part *part);
  bool writeInstrDef(pugi::xml_node node, const engraving::Part *part);
  bool writeEnding(const engraving::Measure *measure);
  bool writeEndingEnd(const engraving::Measure *measure);
  bool writeSystemTrailer(const engraving::Measure *measure);
  bool writeMeasure(const engraving::Measure *measure, int &measureN,
                    bool &isFirst, bool &wasLastIrregular);
  bool writeStaff(const engraving::Staff *staff,
                  const engraving::Measure *measure);
  bool writeLayer(engraving::track_idx_t track, const engraving::Staff *staff,
                  const engraving::Measure *measure);

  // Update note indices
  int updateNoteIndex(int notePitch);

  /**
   * Methods for writing MEI elements within a <layer>
   */
  bool writeArtics(const engraving::Chord *chord);
  bool writeArtic(const engraving::Articulation *articulation);
  bool writeBeamAndTuplet(const engraving::ChordRest *chordRest,
                          bool &closingBeam, bool &closingTuplet,
                          bool &closingBeamInTuplet);
  bool writeBeamAndTupletEnd(bool closingBeam, bool closingTuplet,
                             bool closingBeamInTuplet);
  bool writeBeam(const engraving::Beam *beam,
                 const engraving::ChordRest *chordRest, bool &closing);
  bool writeBTrem(const engraving::TremoloSingleChord *tremolo);
  bool writeClef(const engraving::Clef *clef, const engraving::Measure *measure,
                 const engraving::Segment *seg, int staffN);
  bool writeChord(const engraving::Chord *chord, const engraving::Staff *staff);
  bool writeGraceChord(const engraving::Chord *graceChord,
                       const engraving::Chord *parentChord,
                       const engraving::Staff *staff, bool isAfter);
  bool writeGraceGrp(const engraving::Chord *chord,
                     const engraving::Staff *staff, bool isAfter = false);
  bool writeGraceNote(const engraving::Note *note,
                      const engraving::Chord *graceChord,
                      const engraving::Chord *parentChord,
                      const engraving::Staff *staff, bool isChord,
                      bool isAfter);
  bool writeNote(const engraving::Note *note, const engraving::Chord *chord,
                 const engraving::Staff *staff, bool isChord);
  bool writeMRpt(const engraving::MeasureRepeat *measureRepeat);
  bool writeRest(const engraving::Rest *rest, const engraving::Staff *staff);
  bool writeSyl(const engraving::Lyrics *lyrics, const muse::String &text,
                ElisionType elision);
  bool writeTuplet(const engraving::Tuplet *tuplet,
                   const engraving::EngravingItem *item, bool &closing);
  bool writeVerses(const engraving::ChordRest *chordRest);
  bool writeVerse(const engraving::Lyrics *lyrics);

  double pageHeightInches() const;
  double toBottomLeftInches(double pageYPixels) const;

  std::vector<const engraving::Note *> collectSlurNotes(
      const engraving::Slur *slur) const;

  std::vector<const engraving::Note *> collectHairpinCoveredNotes(
      const engraving::Hairpin *hairpin) const;

  struct TieContextNoteLists {
    std::vector<const engraving::Note *> covered;
    std::vector<const engraving::Note *> terminal;
    std::vector<const engraving::Note *> postTerminal;
  };

  TieContextNoteLists collectTieContextNotes(
      const engraving::Tie *tie) const;

  /**
   * Methods for writing MEI control events (within <measure>)
   */
  bool writeArpeg(const engraving::Arpeggio *arpeggio,
                  const std::string &startid);
  bool writeBreath(const engraving::Breath *breath, const std::string &startid);
  bool writeDir(const engraving::TextBase *dir, const std::string &startid);
  bool writeDir(const engraving::TextLineBase *dir, const std::string &startid);
  bool writeDynam(const engraving::Dynamic *dynamic,
                  const std::string &startid);
  bool writeF(const engraving::FiguredBassItem *figuredBassItem);
  bool writeFb(const engraving::FiguredBass *figuredBass,
               const std::string &startid);
  bool writeFermata(const engraving::Fermata *fermata,
                    const std::string &startid);
  bool writeFermata(const engraving::Fermata *fermata,
                    const libmei::xsdPositiveInteger_List &staffNs,
                    double tstamp);
  bool writeFing(const engraving::Fingering *fing, const std::string &startid);
  bool writeHairpin(const engraving::Hairpin *hairpin,
                    const std::string &startid);
  bool writeHarm(const engraving::Harmony *harmony, const std::string &startid);
  bool writeHarpPedal(const engraving::HarpPedalDiagram *harpPedalDiagram,
                      const std::string &startid);
  bool writeOctave(const engraving::Ottava *ottava, const std::string &startid);
  bool writeOrnament(const engraving::Ornament *ornament,
                     const std::string &startid);
  bool writePedal(const engraving::Pedal *pedal, const std::string &startid);
  bool writeRehearsalMark(const engraving::RehearsalMark *mark,
                          const std::string &startid);
  bool writeRepeatMark(const engraving::Jump *jump,
                       const engraving::Measure *measure);
  bool writeRepeatMark(const engraving::Marker *marker,
                       const engraving::Measure *measure);
  bool writeSlur(const engraving::Slur *slur, const std::string &startid);
  bool writeTempo(const engraving::TempoText *tempoText,
                  const std::string &startid);
  bool writeTie(const engraving::Tie *tie, const std::string &startid);
  bool writeTrill(const engraving::Trill *trill, const std::string &startid);

  /**
   * Methods for writing specific MEI attribute classes within elements
   */
  bool writeBeamTypeAtt(const engraving::ChordRest *chordRest,
                        libmei::AttTyped &typeAtt);
  bool writeStaffIdentAtt(const engraving::ChordRest *chordRest,
                          const engraving::Staff *staff,
                          libmei::AttStaffIdent &staffIdentAtt);
  bool writeStemAtt(const engraving::Chord *chord, libmei::AttStems &stemsAtt);

  /**
   * Helper methods
   */
  bool isCurrentNode(const libmei::Element &element);
  std::list<const engraving::Volta *>
  findVoltasInMeasure(const engraving::Measure *measure);
  void fillControlEventMap(const std::string &xmlId,
                           const engraving::ChordRest *chordRest);
  void collectPendingControlEvents(const engraving::ChordRest *chordRest);
  void flushPendingControlEvents(const std::string &xmlId,
                                 engraving::track_idx_t trackIdx);
  std::string findStartIdFor(const engraving::EngravingItem *item);
  void addToRepeatMarkList(const engraving::EngravingItem *repeatMark,
                           pugi::xml_node node, const std::string &xmlId);
  void addJumpToRepeatMarks();
  bool addFermataToMap(engraving::track_idx_t track,
                       const engraving::Segment *segment,
                       const engraving::Measure *measure);
  std::pair<libmei::xsdPositiveInteger_List, double>
  findTstampFor(const engraving::EngravingItem *item);
  bool isNode(pugi::xml_node node, const muse::String &name);
  pugi::xml_node getLastChordRest(pugi::xml_node node);
  void addNodeToOpenControlEvents(pugi::xml_node node,
                                  const engraving::Spanner *spanner,
                                  const std::string &startid);
  void addEndidToControlEvents();
  bool isLaissezVibrer(const engraving::SymId id);
  /**
   * Full Score Rendering Methods
   */
  void writeSurfaceSizeInches(pugi::xml_node &meiHead);
  void appendSpatiumInches(pugi::xml_node &surface);

  /**
   * Note indexing methods
   */
  void collectNoteEvents(const engraving::Measure *measure);
  void assignIndicesForMeasure(const engraving::Measure *measure);

  /**
   * Methods for generating @xml:ids
   */
  uint32_t hash(uint32_t number, bool reverse);
  std::string baseEncodeInt(uint32_t value, uint8_t base);
  std::string generateHashID();
  std::string getXmlIdFor(const engraving::EngravingItem *item, const char c);
  void resetLayerIDs();
  std::string getSectionXmlId();
  std::string getMeasureXmlId(const engraving::Measure *measure);
  std::string getStaffXmlId();
  std::string getLayerXmlId();
  std::string getLayerXmlIdFor(layerElementCounter elementType);

  /**
   * Helper methods for beat position calculation
   */
  double calculateBeatPosition(const engraving::ChordRest *cr) const;
  double calculateGraceNoteBeatPosition(const engraving::Chord *graceChord,
                                        const engraving::Chord *parentChord,
                                        bool isAfter) const;

  /** A structure of holding a list of repeat marks to be post-process in
   * addJumpToRepeatMarks (currently unused) */
  struct RepeatMark {
    const engraving::EngravingItem *m_repeatMark = nullptr;
    pugi::xml_node m_node;
    std::string m_xmlId;
    std::string m_jumptToLabel;
    std::string m_jumpToXmlId;
  };

  /** The note index map */
  std::multimap<int, int> noteIndexMap = {}; // <key (pitch), note index>

  /** Map each Note* to its precomputed index */
  std::unordered_map<const engraving::Note *, int> m_noteIdxAssignment;
  /** Map of tie start notes to their indices (for cross-measure ties) */
  std::unordered_map<const engraving::Note *, int> m_idxForTieStart;
  /** Map each Note* to the xml:id used when exporting */
  std::unordered_map<const engraving::Note *, std::string> m_noteXmlIdCache;
  /** Stable identities and indices emitted into the coordinated Pianomania MEI. */
  std::vector<PianomaniaNoteRecord> m_pianomaniaNoteRecords;
  bool m_requirePrecomputedPianomaniaIndices = false;
  bool m_includeExportDate = true;
  /** Temporary list of all note-events in the current measure */
  mutable std::unordered_map<const engraving::EngravingItem *, std::string>
      m_exportedXmlIds;
  struct DurEvent {
    int tick; // measure-relative tick (integer, robust for grouping)
    double tstamp;
    const engraving::Note *note;
    const engraving::Chord *indexGroupChord;
  };
  std::vector<DurEvent> m_durEvents;

  /** The Score pointer */
  engraving::Score *m_score = nullptr;

  /** The uid register */
  UIDRegister *m_uids;

  /** MEI xml element */
  pugi::xml_node m_mei;
  /** Current xml element */
  pugi::xml_node m_currentNode;

  /** When writing layers, keep a pointer to the keySig segment (if any) to
   * prepend a scoreDef if necessary */
  const engraving::Segment *m_keySig;
  /** Same for the timeSig segment */
  const engraving::Segment *m_timeSig;
  /** A flag indicating that we have multiple sections in the file */
  bool m_hasSections;

  /** A list of items (first) for which we know the @startid (second)  */
  std::list<std::pair<const engraving::EngravingItem *, std::string>>
      m_startingControlEventList;
  /** A map of items with the @endid they will need to have added */
  std::map<const engraving::EngravingItem *, std::string>
      m_endingControlEventMap;
  /** A map of items with the @plist value they will need to have added */
  std::map<const engraving::EngravingItem *, std::string> m_plistMap;
  /** A map of chord that are a plist of the arpeggio */
  std::map<const engraving::Chord *, const engraving::Arpeggio *>
      m_arpegPlistMap;
  /** A map of elements (e.g., Fermata) to which a tstamp will need to be added
   */
  std::list<std::pair<const engraving::EngravingItem *,
                      std::pair<libmei::xsdPositiveInteger_List, double>>>
      m_tstampControlEventMap;
  /** Control events attached to hidden ChordRests that need the next visible
   * layer element as their @startid. */
  std::list<std::pair<engraving::track_idx_t, const engraving::EngravingItem *>>
      m_pendingControlEventList;
  /** A map of items with the corresponding node (to which the endid from
   * m_endingControlEventMap or plist from m_plistMap will be added) */
  std::map<const engraving::EngravingItem *, pugi::xml_node>
      m_openControlEventMap;

  std::list<MeiExporter::RepeatMark> m_repeatMarks;

  /** Counters for generating xml:ids */
  int m_xmlIDCounter;
  int m_sectionCounter;
  int m_measureCounter;
  int m_staffCounter;
  int m_layerCounter;
  /** Sub counters by elementType */
  std::vector<int> m_layerCounterFor;

  /** map of abbreviations for element within layers */
  inline static const std::map<layerElementCounter, muse::String>
      s_layerXmlIdMap = {{ACCID_L, u"a"},    {BEAM_L, u"b"},
                         {GRACEGRP_L, u"g"}, {SYL_L, u"s"},
                         {VERSE_L, u"v"},    {UNSPECIFIED_L, u"x"}};

  double getArticulationYOffset(const engraving::Articulation *articulation,
                                const engraving::Chord *chord);
  double getDynamicYOffset(const engraving::Dynamic *dynamic);
  double getDirectiveYOffset(const engraving::TextBase *dir);
  double getDirectiveYOffset(const engraving::TextLineBase *dir);
  double getRepeatMarkYOffset(const engraving::TextBase *repeatMark,
                              const engraving::Measure *measure) const;
  double getFermataYOffset(const engraving::Fermata *fermata);
  std::optional<std::pair<double, double>>
  getFingeringOffsets(const engraving::Fingering *fing,
                      const std::string &startid) const;
  double getOrnamentYOffset(const engraving::Ornament *ornament) const;
  double getHairpinYOffset(const engraving::Hairpin *hairpin);
  double getOctaveYOffset(const engraving::Ottava *ottava);
  double getOctaveEndHookHeight(const engraving::Ottava *ottava);
  double getVoltaLineYOffset(const engraving::VoltaSegment *segment) const;
  double getVoltaLabelYOffset(const engraving::VoltaSegment *segment) const;
  double getRestYOffset(const engraving::Rest *rest);
  double getStemLength(const engraving::Chord *chord);

  std::string formatBeatPosition(double beatPosition, int precision = 4) const;
  std::optional<std::pair<double, double>>
  getCenteredInchesFor(const engraving::EngravingItem *item) const;
  void appendCenteredPmPosition(pugi::xml_node node,
                                const engraving::EngravingItem *item) const;
  std::optional<std::array<double, 4>>
  getLineEndpointsInches(const engraving::Spanner *spanner) const;
  bool appendPmLineEndpoints(pugi::xml_node node, const engraving::Spanner *spanner,
                             const char *attributeName) const;
  std::string formatDecimalStr(double value, int precision = 2) const;
};
} // namespace mu::iex::mei

#endif // MU_IMPORTEXPORT_MEIEXPORTER_H
