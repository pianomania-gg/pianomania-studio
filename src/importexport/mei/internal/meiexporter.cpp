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

#include "meiexporter.h"

#include <QString>
#include <iomanip>
#include <algorithm>
#include <optional>
#include <tuple>
#include <random>
#include <array>
#include <unordered_set>

#include "global/containers.h"

#include "log.h"
#include "types/datetime.h"

#include "engraving/dom/arpeggio.h"
#include "engraving/dom/barline.h"
#include "engraving/dom/beam.h"
#include "engraving/dom/box.h"
#include "engraving/dom/bracket.h"
#include "engraving/dom/breath.h"
#include "engraving/dom/chord.h"
#include "engraving/compat/midi/compatmidirenderinternal.h"
#include "engraving/dom/chordrest.h"
#include "engraving/dom/clef.h"
#include "engraving/dom/dynamic.h"
#include "engraving/dom/fermata.h"
#include "engraving/dom/figuredbass.h"
#include "engraving/dom/fingering.h"
#include "engraving/dom/glissando.h"
#include "engraving/dom/hairpin.h"
#include "engraving/dom/harmony.h"
#include "engraving/dom/harppedaldiagram.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/jump.h"
#include "engraving/infrastructure/eid.h"
#include "engraving/dom/keysig.h"
#include "engraving/dom/laissezvib.h"
#include "engraving/dom/lyrics.h"
#include "engraving/dom/marker.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/measurerepeat.h"
#include "engraving/dom/mscore.h"
#include "engraving/dom/note.h"
#include "engraving/dom/ornament.h"
#include "engraving/dom/ottava.h"
#include "engraving/dom/page.h"
#include "engraving/dom/part.h"
#include "engraving/dom/pedal.h"
#include "engraving/dom/rubatozone.h"
#include "engraving/dom/rehearsalmark.h"
#include "engraving/dom/rest.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/sig.h"
#include "engraving/dom/slur.h"
#include "engraving/dom/staff.h"
#include "engraving/dom/stem.h"
#include "engraving/dom/system.h"
#include "engraving/dom/tempotext.h"
#include "engraving/dom/text.h"
#include "engraving/dom/textlinebase.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/timesig.h"
#include "engraving/dom/trill.h"
#include "engraving/dom/tuplet.h"
#include "engraving/dom/volta.h"
#include "engraving/rendering/score/systemlayout.h"
#include "engraving/rendering/score/paint.h"
#include "engraving/style/style.h"

#include "thirdparty/libmei/cmn.h"
#include "thirdparty/libmei/fingering.h"
#include "thirdparty/libmei/harmony.h"
#include "thirdparty/libmei/lyrics.h"
#include "thirdparty/libmei/midi.h"
#include "thirdparty/libmei/shared.h"

using namespace mu::iex::mei;

static constexpr double PIANOMANIA_MEI_DPI = 360.0;
using namespace mu::engraving;

// Number of spaces for the XML indentation. Set to 0 for tabs
#define MEI_INDENT 3

// Use counter-based IDs for layer elements
#define MEI_COUNTER_BASED_IDS false

namespace {
bool shouldExportStaff(const Staff *staff) {
  const Part *part = staff ? staff->part() : nullptr;
  return staff && staff->show() && part && part->show();
}

bool shouldExportNote(const Note *note) {
  return note && note->visible() && shouldExportStaff(note->staff());
}

const char *pianomaniaHeldPulseValue(Note::PianomaniaHeldNotePulse pulse) {
  switch (pulse) {
  case Note::PianomaniaHeldNotePulse::Quarter:
    return "quarter";
  case Note::PianomaniaHeldNotePulse::Eighth:
    return "eighth";
  case Note::PianomaniaHeldNotePulse::Sixteenth:
    return "sixteenth";
  case Note::PianomaniaHeldNotePulse::ThirtySecond:
    return "thirty-second";
  case Note::PianomaniaHeldNotePulse::SixtyFourth:
    return "sixty-fourth";
  case Note::PianomaniaHeldNotePulse::None:
    return nullptr;
  }

  return nullptr;
}

void appendPianomaniaHeldPulseAttributes(pugi::xml_node node,
                                         const Note *note) {
  if (!note->pianomaniaHeldNote()) {
    return;
  }

  const char *pulse = pianomaniaHeldPulseValue(note->pianomaniaHeldNotePulse());
  if (!pulse) {
    return;
  }

  node.append_attribute("heldPulse") = pulse;
  node.append_attribute("heldPulseTriplet") =
      note->pianomaniaHeldNotePulseTriplet() ? "true" : "false";
}

bool appendPianomaniaHeldPitchCurveAttribute(pugi::xml_node node,
                                             const Note *note) {
  const PianomaniaHeldNotePitchCurve &curve = note->pianomaniaHeldNotePitchCurve();
  const std::vector<Note *> tieChain = note->tiedNotes();
  const bool chainHasCurve = std::any_of(
      tieChain.cbegin(), tieChain.cend(), [](const Note *member) {
        return member && !member->pianomaniaHeldNotePitchCurve().empty();
      });
  if (curve.empty()) {
    if (!chainHasCurve) {
      return true;
    }
    LOGE() << "MEI export found an incomplete Pianomania Held Note pitch curve tie chain";
    return false;
  }
  if (!note->pianomaniaHeldNote() ||
      !Note::isValidPianomaniaHeldNotePitchCurve(curve, note->pianomaniaHeldNoteDurationTicks())) {
    LOGE() << "MEI export found an invalid Pianomania Held Note pitch curve";
    return false;
  }
  if (std::any_of(tieChain.cbegin(), tieChain.cend(), [&curve](const Note *member) {
        return !member || !member->pianomaniaHeldNote() || member->pianomaniaHeldNotePitchCurve() != curve;
      })) {
    LOGE() << "MEI export found inconsistent Pianomania Held Note pitch curves in a tie chain";
    return false;
  }

  std::string value;
  for (const PianomaniaHeldNotePitchCurvePoint &point : curve) {
    if (!value.empty()) {
      value += ';';
    }
    value += std::to_string(point.scoreTick);
    value += ':';
    value += std::to_string(point.pitchCents);
    value += ':';
    value += std::to_string(point.slopeCentsPerQuarter);
  }
  node.append_attribute("pm:heldPitchCurveV3") = value.c_str();
  return true;
}

std::vector<const Note *> visibleNotes(const Chord *chord) {
  std::vector<const Note *> notesToExport;
  if (!chord) {
    return notesToExport;
  }

  notesToExport.reserve(chord->notes().size());
  for (const Note *note : chord->notes()) {
    if (shouldExportNote(note)) {
      notesToExport.push_back(note);
    }
  }

  return notesToExport;
}

bool shouldExportChordRest(const ChordRest *chordRest) {
  if (!chordRest) {
    return false;
  }
  if (!shouldExportStaff(chordRest->staff())) {
    return false;
  }
  if (chordRest->isChord()) {
    return !visibleNotes(toChord(chordRest)).empty();
  }
  if (chordRest->isRest()) {
    const Rest *rest = toRest(chordRest);
    return rest->visible() && !rest->isGap();
  }
  return chordRest->visible();
}

bool shouldExportDurationElement(const DurationElement *element) {
  return element && element->isChordRest() &&
         shouldExportChordRest(toChordRest(element));
}

const ChordRest *findExportedSpannerEndpointAnchor(Spanner *spanner,
                                                    bool start) {
  if (!spanner || !spanner->score()) {
    return nullptr;
  }

  const ChordRest *endpoint = start ? spanner->startCR() : spanner->endCR();
  if (shouldExportChordRest(endpoint)) {
    return endpoint;
  }

  const Segment *segment = endpoint
                               ? endpoint->segment()
                               : (start ? spanner->startSegment()
                                        : spanner->endSegment());
  const track_idx_t preferredTrack = endpoint
                                         ? endpoint->track()
                                         : (start ? spanner->track()
                                                  : spanner->effectiveTrack2());
  if (!segment || preferredTrack >= spanner->score()->ntracks()) {
    return nullptr;
  }

  const track_idx_t firstTrack = staff2track(track2staff(preferredTrack));
  const track_idx_t endTrack =
      std::min(firstTrack + VOICES, spanner->score()->ntracks());
  const track_idx_t preferredVoice = preferredTrack - firstTrack;
  const auto exportedChordRestAtTrack = [segment](track_idx_t track) {
    const EngravingItem *item = segment->element(track);
    if (!item || !item->isChordRest()) {
      return static_cast<const ChordRest *>(nullptr);
    }

    const ChordRest *candidate = toChordRest(item);
    return shouldExportChordRest(candidate) ? candidate : nullptr;
  };

  for (track_idx_t distance = 0; distance < VOICES; ++distance) {
    if (distance <= preferredVoice) {
      const track_idx_t lowerTrack = preferredTrack - distance;
      if (const ChordRest *candidate = exportedChordRestAtTrack(lowerTrack)) {
        return candidate;
      }
    }

    const track_idx_t higherTrack = preferredTrack + distance;
    if (distance > 0 && higherTrack < endTrack) {
      if (const ChordRest *candidate = exportedChordRestAtTrack(higherTrack)) {
        return candidate;
      }
    }
  }
  return nullptr;
}

bool shouldExportRubatoBoundaryAnchor(const ChordRest *chordRest) {
  if (!shouldExportChordRest(chordRest) || chordRest->generated()) {
    return false;
  }
  const Measure *measure = chordRest->measure();
  return measure &&
         measure->measureRepeatNumMeasures(chordRest->staffIdx()) != 1;
}

const ChordRest *findExportedRubatoBoundaryAnchor(const Measure *measure,
                                                   track_idx_t preferredTrack,
                                                   bool findFirst) {
  if (!measure || !measure->score()) {
    return nullptr;
  }

  auto findInSegment = [preferredTrack, measure](const Segment *segment) {
    auto chordRestAtTrack = [segment](track_idx_t track) {
      const EngravingItem *item = segment->element(track);
      if (!item || !item->isChordRest()) {
        return static_cast<const ChordRest *>(nullptr);
      }
      const ChordRest *chordRest = toChordRest(item);
      return shouldExportRubatoBoundaryAnchor(chordRest) ? chordRest : nullptr;
    };

    if (preferredTrack < measure->score()->ntracks()) {
      if (const ChordRest *preferred = chordRestAtTrack(preferredTrack)) {
        return preferred;
      }
    }
    for (track_idx_t track = 0; track < measure->score()->ntracks(); ++track) {
      if (track == preferredTrack) {
        continue;
      }
      if (const ChordRest *candidate = chordRestAtTrack(track)) {
        return candidate;
      }
    }
    return static_cast<const ChordRest *>(nullptr);
  };

  for (const Segment *segment =
           findFirst ? measure->first(SegmentType::ChordRest)
                     : measure->last(SegmentType::ChordRest);
       segment;
       segment = findFirst ? segment->next(SegmentType::ChordRest)
                           : segment->prev(SegmentType::ChordRest)) {
    if (const ChordRest *anchor = findInSegment(segment)) {
      return anchor;
    }
  }
  return nullptr;
}

std::optional<std::pair<const HairpinSegment *, const System *>>
resolvedHairpinLayout(const Hairpin *hairpin) {
  if (!hairpin || hairpin->segmentsEmpty()) {
    LOGE() << "MEI export requires laid-out segments for a hairpin";
    return std::nullopt;
  }

  const SpannerSegment *firstSegment = hairpin->frontSegment();
  if (!firstSegment || !firstSegment->isHairpinSegment()) {
    LOGE() << "MEI export requires a laid-out first HairpinSegment";
    return std::nullopt;
  }

  const HairpinSegment *hairpinSegment = toHairpinSegment(firstSegment);
  const System *system = hairpinSegment->system();
  if (!system) {
    LOGE() << "MEI export requires a System for the first HairpinSegment";
    return std::nullopt;
  }

  return std::make_pair(hairpinSegment, system);
}

bool appendResolvedCenterBetweenStaves(pugi::xml_node node,
                                       const EngravingItem *item) {
  if (!item || (!item->isDynamic() && !item->isExpression())) {
    return true;
  }

  const EngravingObject *parent = item->explicitParent();
  if (!parent || parent->type() != ElementType::SEGMENT) {
    LOGE() << "MEI export requires a Segment for a centered directive";
    return false;
  }

  const System *system = toSegment(parent)->system();
  if (!system) {
    LOGE() << "MEI export requires a System for a centered directive";
    return false;
  }

  const bool centered = engraving::rendering::score::SystemLayout::
      elementShouldBeCenteredBetweenStaves(item, system);
  node.append_attribute("centerBetweenStaves") = centered ? "true" : "false";
  return true;
}

size_t exportedBeamElementCount(const Beam *beam) {
  return beam ? std::count_if(beam->elements().cbegin(),
                              beam->elements().cend(),
                              shouldExportChordRest)
              : 0;
}

size_t exportedTupletElementCount(const Tuplet *tuplet) {
  return tuplet ? std::count_if(tuplet->elements().cbegin(),
                                tuplet->elements().cend(),
                                shouldExportDurationElement)
                : 0;
}

bool isFirstExportedBeamElement(const Beam *beam,
                                const ChordRest *chordRest) {
  if (!beam) {
    return false;
  }
  const auto &elements = beam->elements();
  const auto first =
      std::find_if(elements.cbegin(), elements.cend(), shouldExportChordRest);
  return first != elements.cend() && *first == chordRest;
}

bool isFirstExportedTupletElement(const Tuplet *tuplet,
                                  const ChordRest *chordRest) {
  if (!tuplet) {
    return false;
  }
  const auto &elements = tuplet->elements();
  const auto first = std::find_if(elements.cbegin(), elements.cend(),
                                  shouldExportDurationElement);
  return first != elements.cend() && *first == chordRest;
}

const BarLine *firstBarlineInSegment(const Segment *segment) {
  if (!segment) {
    return nullptr;
  }

  for (const EngravingItem *item : segment->elist()) {
    if (item && item->isBarLine()) {
      return toBarLine(item);
    }
  }

  return nullptr;
}
} // namespace

double MeiExporter::pageHeightInches() const {
  if (!m_score) {
    return 0.0;
  }

  return m_score->style().styleD(Sid::pageHeight);
}

double MeiExporter::toBottomLeftInches(double pageYPixels) const {
  double heightInches = pageHeightInches();
  if (heightInches <= 0.0) {
    return pageYPixels / DPI;
  }

  return heightInches - (pageYPixels / DPI);
}

std::optional<std::pair<double, double>>
MeiExporter::getCenteredInchesFor(const EngravingItem *item) const {
  if (!item) {
    return std::nullopt;
  }

  RectF bbox = item->pageBoundingRect();
  double xInches = (bbox.x() + (bbox.width() / 2.0)) / DPI;
  double yInches = toBottomLeftInches(bbox.y() + (bbox.height() / 2.0));

  return std::make_pair(xInches, yInches);
}

void MeiExporter::appendCenteredPmPosition(pugi::xml_node node,
                                           const EngravingItem *item) const {
  if (!node) {
    return;
  }

  const auto centered = getCenteredInchesFor(item);
  if (!centered.has_value()) {
    return;
  }

  const std::string xStr = formatDecimalStr(centered->first, 3);
  const std::string yStr = formatDecimalStr(centered->second, 3);
  const std::string xyStr = xStr + std::string(",") + yStr;

  node.append_attribute("pm:xy") = xyStr.c_str();
}

bool MeiExporter::appendPmTupletGeometry(pugi::xml_node node,
                                         const Tuplet *tuplet) const {
  if (!node || !tuplet) {
    return false;
  }

  const bool drawVisible = tuplet->visible() && !tuplet->ldata()->isSkipDraw();
  const bool numberVisible = drawVisible && tuplet->number() &&
                             tuplet->number()->visible() &&
                             !tuplet->number()->ldata()->isSkipDraw();
  const bool bracketVisible = drawVisible && tuplet->hasBracket();
  node.append_attribute("pm:tuplet-geometry-version") = "1";
  node.append_attribute("pm:tuplet-placement") =
      tuplet->isUp() ? "above" : "below";
  node.append_attribute("pm:tuplet-number-visible") =
      numberVisible ? "true" : "false";
  node.append_attribute("pm:tuplet-bracket-visible") =
      bracketVisible ? "true" : "false";

  const auto pointToInches = [this](const PointF &pagePoint) {
    return std::make_pair(pagePoint.x() / DPI,
                          toBottomLeftInches(pagePoint.y()));
  };
  const auto formatPoint = [this, &pointToInches](const PointF &pagePoint) {
    const auto point = pointToInches(pagePoint);
    return formatDecimalStr(point.first, 3) + std::string(",") +
           formatDecimalStr(point.second, 3);
  };
  const PointF tupletPagePosition = tuplet->pagePos();
  const auto formatSegment = [&formatPoint, &tupletPagePosition](
                                 const PointF &start,
                                 const PointF &end) {
    return formatPoint(tupletPagePosition + start) + std::string(",") +
           formatPoint(tupletPagePosition + end);
  };

  if (numberVisible) {
    const RectF numberBounds = tuplet->number()->pageBoundingRect();
    const PointF numberCenter(numberBounds.x() + numberBounds.width() * 0.5,
                              numberBounds.y() + numberBounds.height() * 0.5);
    const std::string numberCenterValue = formatPoint(numberCenter);
    node.append_attribute("pm:tuplet-number-center") =
        numberCenterValue.c_str();
  }

  if (!bracketVisible) {
    return true;
  }

  std::string segmentValue;
  std::string hookValue;
  if (numberVisible) {
    segmentValue = formatSegment(tuplet->bracketL[1], tuplet->bracketL[2]) +
                   std::string(";") +
                   formatSegment(tuplet->bracketR[0], tuplet->bracketR[1]);
    hookValue = formatSegment(tuplet->bracketL[0], tuplet->bracketL[1]) +
                std::string(";") +
                formatSegment(tuplet->bracketR[1], tuplet->bracketR[2]);
  } else {
    segmentValue = formatSegment(tuplet->bracketL[1], tuplet->bracketL[2]);
    hookValue = formatSegment(tuplet->bracketL[0], tuplet->bracketL[1]) +
                std::string(";") +
                formatSegment(tuplet->bracketL[2], tuplet->bracketL[3]);
  }

  node.append_attribute("pm:tuplet-bracket-segments") = segmentValue.c_str();
  node.append_attribute("pm:tuplet-bracket-hooks") = hookValue.c_str();
  return true;
}

std::optional<std::array<double, 4>>
MeiExporter::getLineEndpointsInches(const Spanner *spanner) const {
  if (!spanner || spanner->segmentsEmpty()) {
    return std::nullopt;
  }

  const SpannerSegment *firstSegment = spanner->frontSegment();
  const SpannerSegment *lastSegment = spanner->backSegment();
  if (!firstSegment || !lastSegment) {
    return std::nullopt;
  }

  PointF startPos = firstSegment->pagePos();
  PointF endPos = lastSegment->pagePos() + lastSegment->pos2();

  double x1 = startPos.x() / DPI;
  double y1 = toBottomLeftInches(startPos.y());
  double x2 = endPos.x() / DPI;
  double y2 = toBottomLeftInches(endPos.y());

  return std::array<double, 4>{x1, y1, x2, y2};
}

bool MeiExporter::appendPmLineEndpoints(pugi::xml_node node,
                                        const Spanner *spanner,
                                        const char *attributeName) const {
  if (!node || !attributeName) {
    return false;
  }

  auto endpoints = getLineEndpointsInches(spanner);
  if (!endpoints.has_value()) {
    return false;
  }

  const auto &coords = endpoints.value();
  std::string xy = formatDecimalStr(coords[0], 3) + std::string(",") +
                   formatDecimalStr(coords[1], 3) + std::string(",") +
                   formatDecimalStr(coords[2], 3) + std::string(",") +
                   formatDecimalStr(coords[3], 3);

  node.append_attribute(attributeName) = xy.c_str();
  return true;
}

int MeiExporter::updateNoteIndex(int notePitch) {
  // Use ppitch() to get the actual heard pitch including ottava shifts
  int heardPitch = notePitch;
  auto it = noteIndexMap.find(heardPitch);
  if (it != noteIndexMap.end()) {
    return ++it->second;
  }
  // if not found, create a new entry
  noteIndexMap.insert({heardPitch, 0});
  return 0;
}

/**
 * Write the Score to the destination file.
 * Return false on error.
 */

bool MeiExporter::write(std::string &meiData) {
  const bool useMuseScoreIds = configuration()->meiUseMuseScoreIds();

  // Reset the note indices
  noteIndexMap = {};
  m_exportedXmlIds.clear();
  m_noteXmlIdCache.clear();
  m_pianomaniaNoteRecords.clear();

  m_uids = UIDRegister::instance();
  m_xmlIDCounter = 0;

  m_hasSections = false;

  m_sectionCounter = 0;
  m_measureCounter = 0;
  m_staffCounter = 0;
  m_layerCounter = 0;
  m_layerCounterFor.resize(UNSPECIFIED_L + 1);
  this->resetLayerIDs();

  try {
    pugi::xml_document meiDoc;

    pugi::xml_node decl = meiDoc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "UTF-8";

    // schema processing instruction
    std::string schema = "https://music-encoding.org/schema/5.1/mei-basic.rng";
    decl = meiDoc.append_child(pugi::node_declaration);
    decl.set_name("xml-model");
    decl.append_attribute("href") = schema.c_str();
    decl.append_attribute("type") = "application/xml";
    decl.append_attribute("schematypens") =
        "http://relaxng.org/ns/structure/1.0";

    decl = meiDoc.append_child(pugi::node_declaration);
    decl.set_name("xml-model");
    decl.append_attribute("href") = schema.c_str();
    decl.append_attribute("type") = "application/xml";
    decl.append_attribute("schematypens") =
        "http://purl.oclc.org/dsdl/schematron";

    m_mei = meiDoc.append_child("mei");
    m_mei.append_attribute("xmlns") = "http://www.music-encoding.org/ns/mei";

    // Add Pianomania namespace
    m_mei.append_attribute("xmlns:pm") = "http://pianomania.gg/ns/mei";

    // Option to use MuseScore Ids has priority
    if (useMuseScoreIds) {
      std::stringstream xmlId;
      EID eid = m_score->masterScore()->eid();
      if (!eid.isValid()) {
        eid = m_score->masterScore()->assignNewEID();
      }
      String eidStr = String::fromStdString(eid.toStdString().c_str());
      xmlId << "mscore-"
            << eidStr.replace('/', '.').replace('+', '-').toStdString();
      m_mei.append_attribute("xml:id") = xmlId.str().c_str();
    }
    // Otherwise check if we have a metaTag
    else {
      // Save xml:id metaTag's as mei@xml:id
      String xmlId = m_score->metaTag(u"xml:id");
      if (!xmlId.isEmpty()) {
        m_mei.append_attribute("xml:id") = xmlId.toStdString().c_str();
      }
    }

    libmei::AttConverter converter;
    libmei::meiVersion_MEIVERSION meiVersion =
        libmei::meiVersion_MEIVERSION_5_1plusbasic;
    m_mei.append_attribute("meiversion") =
        (converter.MeiVersionMeiversionToStr(meiVersion)).c_str();

    if (!this->validateRubatoZones()) {
      return false;
    }
    if (!this->validatePianomaniaHeldPitchCurves()) {
      return false;
    }
    if (!this->validateDanceShowSpans()) {
      return false;
    }
    this->prepareSpannerEndpointAnchors();

    this->writeHeader();

    this->writeScore();

    // Currently not used. To be enabled for unfolding MuseScore Jumps into
    // `@jumpto` MEI attribute if it becomes available on MEI repeatMark
    // this->addJumpToRepeatMarks();

    unsigned int output_flags = pugi::format_default;

    // Tabulation of MEI_INDENT * spaces (tabs if 0)
    std::string indent = MEI_INDENT ? std::string(MEI_INDENT, ' ') : "\t";
    std::stringstream strStream;
    meiDoc.save(strStream, indent.c_str(), output_flags);
    meiData = strStream.str();
  } catch (char *str) {
    UNUSED(str);
    // Do something with the error message
    return false;
  }

  return true;
}

//---------------------------------------------------------
//   convert
//---------------------------------------------------------

/**
 * Pianomania: reject overlapping or nested rubato zones. Zone gameplay
 * semantics require disjoint spans, so the export fails instead of emitting
 * an ambiguous score.
 */

bool MeiExporter::validateRubatoZones() {
  m_rubatoZoneStartAnchors.clear();
  m_rubatoZoneEndAnchors.clear();

  std::vector<Spanner *> zones;
  auto spanners = m_score->spannerMap().findOverlapping(
      0, m_score->endTick().ticks());
  for (auto interval : spanners) {
    Spanner *spanner = interval.value;
    if (spanner && spanner->isRubatoZone()) {
      zones.push_back(spanner);
    }
  }
  std::sort(zones.begin(), zones.end(),
            [](const Spanner *a, const Spanner *b) {
              return a->tick() < b->tick();
  });
  for (Spanner *zone : zones) {
    const Measure *startMeasure = m_score->tick2measureMM(zone->tick());
    const Measure *endMeasure = startMeasure;
    while (endMeasure && endMeasure->endTick() < zone->tick2()) {
      endMeasure = endMeasure->nextMeasureMM();
    }
    if (!startMeasure || !endMeasure || zone->tick() != startMeasure->tick() ||
        zone->tick2() != endMeasure->endTick()) {
      LOGE() << "Rubato zone ticks " << zone->tick().ticks() << ".."
             << zone->tick2().ticks()
             << " must start at a measure boundary and end at a measure boundary.";
      return false;
    }

    const ChordRest *startAnchor = findExportedRubatoBoundaryAnchor(
        startMeasure, zone->track(), true);
    const ChordRest *endAnchor = findExportedRubatoBoundaryAnchor(
        endMeasure, zone->track(), false);
    if (!startAnchor || !endAnchor) {
      LOGE() << "Rubato zone ticks " << zone->tick().ticks() << ".."
             << zone->tick2().ticks()
             << " require exported ChordRest anchors in both boundary measures.";
      return false;
    }
    const RubatoZone *rubatoZone = dynamic_cast<const RubatoZone *>(zone);
    IF_ASSERT_FAILED(rubatoZone) { return false; }
    m_rubatoZoneStartAnchors[rubatoZone] = startAnchor;
    m_rubatoZoneEndAnchors[rubatoZone] = endAnchor;
  }
  for (size_t i = 1; i < zones.size(); ++i) {
    const Spanner *previous = zones[i - 1];
    const Spanner *current = zones[i];
    if (current->tick() < previous->tick2()) {
      LOGE() << "Rubato zones overlap: ticks " << previous->tick().ticks()
             << ".." << previous->tick2().ticks() << " and "
             << current->tick().ticks() << ".." << current->tick2().ticks()
             << ". Rubato zones must be disjoint.";
      return false;
    }
  }
  return true;
}

bool MeiExporter::validatePianomaniaHeldPitchCurves() {
  bool valid = true;
  m_score->scanElements([&valid](EngravingItem *item) {
    if (!valid || !item || !item->isNote()) {
      return;
    }

    const Note *note = toNote(item);
    const PianomaniaHeldNotePitchCurve &curve = note->pianomaniaHeldNotePitchCurve();
    const std::vector<Note *> tieChain = note->tiedNotes();
    const bool chainHasCurve = std::any_of(
        tieChain.cbegin(), tieChain.cend(), [](const Note *member) {
          return member && !member->pianomaniaHeldNotePitchCurve().empty();
        });
    if (curve.empty()) {
      valid = !chainHasCurve;
    } else {
      valid = note->pianomaniaHeldNote() &&
              Note::isValidPianomaniaHeldNotePitchCurve(curve, note->pianomaniaHeldNoteDurationTicks()) &&
              std::none_of(tieChain.cbegin(), tieChain.cend(),
                           [&curve](const Note *member) {
                             return !member || !member->pianomaniaHeldNote() ||
                                    member->pianomaniaHeldNotePitchCurve() != curve;
                           });
    }
    if (!valid) {
      LOGE() << "MEI export found invalid or inconsistent Pianomania Held Note pitch curve data";
    }
  });
  return valid;
}

/**
 * Pianomania: reject same-type overlaps between dance-show effect spans
 * (pyro, laser). Cross-type overlap, including with rubato zones, is
 * allowed. Pyro reference anchors may be any exported ChordRest because
 * the authored tick positions are exported separately. Laser retains its
 * attacked-chord anchor rule.
 */

/**
 * A dance-show anchor chord must actually attack: a chord whose notes all
 * continue ties never binds a MIDI note bar, so Unity could not place the
 * span on the note timeline.
 */
bool isExportedAttackedChord(const ChordRest *chordRest) {
  if (!chordRest || !chordRest->isChord() ||
      !shouldExportChordRest(chordRest)) {
    return false;
  }
  for (const Note *note : toChord(chordRest)->notes()) {
    if (note && !note->tieBack()) {
      return true;
    }
  }
  return false;
}

const ChordRest *findExportedLaserChordAnchor(Spanner *span, bool start) {
  const ChordRest *endpoint = findExportedSpannerEndpointAnchor(span, start);
  if (isExportedAttackedChord(endpoint)) {
    return endpoint;
  }
  if (!endpoint || !span->score()) {
    return nullptr;
  }

  const Segment *segment = endpoint->segment();
  if (!segment) {
    return nullptr;
  }
  const track_idx_t firstTrack = staff2track(track2staff(endpoint->track()));
  const track_idx_t endTrack =
      std::min(firstTrack + VOICES, span->score()->ntracks());

  // Search the endpoint segment first, then walk toward the span's interior
  // (forward for the start anchor, backward for the end anchor) until an
  // attacked chord in the span's staff is found inside the span's ticks.
  const Segment *candidateSegment = segment;
  while (candidateSegment) {
    if (candidateSegment->isChordRestType()) {
      const Fraction candidateTick = candidateSegment->tick();
      if (candidateTick < span->tick() || candidateTick >= span->tick2()) {
        break;
      }
      for (track_idx_t track = firstTrack; track < endTrack; ++track) {
        const EngravingItem *item = candidateSegment->element(track);
        if (!item || !item->isChordRest()) {
          continue;
        }
        const ChordRest *candidate = toChordRest(item);
        if (isExportedAttackedChord(candidate)) {
          return candidate;
        }
      }
    }
    candidateSegment = start
                           ? candidateSegment->next1(SegmentType::ChordRest)
                           : candidateSegment->prev1(SegmentType::ChordRest);
  }
  return nullptr;
}

const ChordRest *findExportedDanceShowAnchorAtSegment(const Segment *segment,
                                                      bool attackedOnly) {
  if (!segment || !segment->score()) {
    return nullptr;
  }

  for (track_idx_t track = 0; track < segment->score()->ntracks(); ++track) {
    const EngravingItem *item = segment->element(track);
    if (!item || !item->isChordRest()) {
      continue;
    }
    const ChordRest *candidate = toChordRest(item);
    if (attackedOnly ? isExportedAttackedChord(candidate)
                     : shouldExportChordRest(candidate)) {
      return candidate;
    }
  }
  return nullptr;
}

const ChordRest *findExportedPyroAnchor(Spanner *span, bool start) {
  if (!span || !span->score()) {
    return nullptr;
  }

  const Fraction boundaryTick = start ? span->tick() : span->tick2();
  const Segment *boundarySegment = span->score()->tick2segment(
      boundaryTick, false, SegmentType::ChordRest, false);
  if (const ChordRest *anchor =
          findExportedDanceShowAnchorAtSegment(boundarySegment, true)) {
    return anchor;
  }
  if (const ChordRest *anchor =
          findExportedDanceShowAnchorAtSegment(boundarySegment, false)) {
    return anchor;
  }

  const ChordRest *interiorAnchor = nullptr;
  for (const Segment *segment =
           span->score()->firstSegment(SegmentType::ChordRest);
       segment; segment = segment->next1(SegmentType::ChordRest)) {
    if (segment->tick() < span->tick()) {
      continue;
    }
    if (segment->tick() >= span->tick2()) {
      break;
    }
    const ChordRest *candidate =
        findExportedDanceShowAnchorAtSegment(segment, true);
    if (!candidate) {
      candidate = findExportedDanceShowAnchorAtSegment(segment, false);
    }
    if (!candidate) {
      continue;
    }
    if (start) {
      return candidate;
    }
    interiorAnchor = candidate;
  }
  return interiorAnchor;
}

bool MeiExporter::validateDanceShowSpans() {
  m_danceShowStartAnchors.clear();
  m_danceShowEndAnchors.clear();

  std::vector<Spanner *> spans;
  auto spanners = m_score->spannerMap().findOverlapping(
      0, m_score->endTick().ticks());
  for (auto interval : spanners) {
    Spanner *spanner = interval.value;
    if (spanner && (spanner->isPyroSpan() || spanner->isLaserSpan())) {
      spans.push_back(spanner);
    }
  }
  std::sort(spans.begin(), spans.end(),
            [](const Spanner *a, const Spanner *b) {
              return a->tick() < b->tick();
  });

  for (Spanner *span : spans) {
    const ChordRest *startAnchor = span->isPyroSpan()
                                       ? findExportedPyroAnchor(span, true)
                                       : findExportedLaserChordAnchor(span, true);
    const ChordRest *endAnchor = span->isPyroSpan()
                                     ? findExportedPyroAnchor(span, false)
                                     : findExportedLaserChordAnchor(span, false);
    if (!startAnchor || !endAnchor) {
      LOGE() << span->typeName() << " ticks " << span->tick().ticks() << ".."
             << span->tick2().ticks()
             << " requires resolvable exported reference anchors.";
      return false;
    }
    m_danceShowStartAnchors[span] = startAnchor;
    m_danceShowEndAnchors[span] = endAnchor;
  }

  for (size_t i = 1; i < spans.size(); ++i) {
    const Spanner *current = spans[i];
    for (size_t j = 0; j < i; ++j) {
      const Spanner *previous = spans[j];
      if (previous->type() != current->type()) {
        continue;
      }
      if (current->tick() < previous->tick2()) {
        LOGE() << current->typeName() << " spans overlap: ticks "
               << previous->tick().ticks() << ".." << previous->tick2().ticks()
               << " and " << current->tick().ticks() << ".."
               << current->tick2().ticks()
               << ". Spans of the same effect type must be disjoint.";
        return false;
      }
    }
  }
  return true;
}

void MeiExporter::prepareSpannerEndpointAnchors() {
  m_spannerStartAnchors.clear();
  m_spannerEndAnchors.clear();

  for (const auto &[tick, spanner] : m_score->spannerMap().map()) {
    UNUSED(tick);
    if (!spanner ||
        !(spanner->isHairpin() || spanner->isOttava() ||
          spanner->isPedal() || spanner->isSlur() || spanner->isTrill())) {
      continue;
    }

    if (const ChordRest *anchor =
            findExportedSpannerEndpointAnchor(spanner, true)) {
      m_spannerStartAnchors.emplace(anchor, spanner);
    }
    if (const ChordRest *anchor =
            findExportedSpannerEndpointAnchor(spanner, false)) {
      m_spannerEndAnchors.emplace(anchor, spanner);
    }
  }
}

/**
 * Write the MEI header.
 * Look for the meiHead custom meta tag (given when importing an MEI file into
 * MuseScore). Otherwise, generate the header using the common meta tags.
 */

bool MeiExporter::writeHeader() {
  String headStr = m_score->metaTag(u"meiHead");
  if (headStr.size() > 0) {
    pugi::xml_document docHead;
    docHead.load_string(headStr.toStdString().c_str());
    m_mei.append_copy(docHead.first_child());
  } else {
    // create header
    pugi::xml_node meiHead = m_mei.append_child("meiHead");
    pugi::xml_node fileDesc = meiHead.append_child("fileDesc");
    pugi::xml_node titleStmt = fileDesc.append_child("titleStmt");
    pugi::xml_node title = titleStmt.append_child("title");
    if (!m_score->metaTag(u"workTitle").isEmpty()) {
      title.text().set(m_score->metaTag(u"workTitle").toStdString().c_str());
      title.append_attribute("type") = "main";
    }
    if (!m_score->metaTag(u"subtitle").isEmpty()) {
      pugi::xml_node subtitle = titleStmt.append_child("title");
      subtitle.text().set(m_score->metaTag(u"subtitle").toStdString().c_str());
      subtitle.append_attribute("type") = "subordinate";
    }

    pugi::xml_node respStmt;
    StringList persNames;
    // the creator types commonly found in MusicXML
    persNames << u"arranger" << u"composer" << u"lyricist" << u"translator";
    for (String tagName : persNames) {
      String persName = m_score->metaTag(tagName);
      if (!persName.isEmpty()) {
        if (!respStmt) {
          respStmt = titleStmt.append_child("respStmt");
        }
        pugi::xml_node persNameNode = respStmt.append_child("persName");
        persNameNode.text().set(persName.toStdString().c_str());
        persNameNode.append_attribute("role") = tagName.toStdString().c_str();
      }
    }

    pugi::xml_node pubStmt = fileDesc.append_child("pubStmt");
    if (m_includeExportDate) {
      pugi::xml_node date = pubStmt.append_child("date");
      String dateStr = muse::DateTime::currentDateTime().toString();
      date.append_attribute("isodate") = dateStr.toStdString().c_str();
    }

    if (!m_score->metaTag(u"copyright").isEmpty()) {
      pugi::xml_node availability = pubStmt.append_child("availability");
      availability.text().set(
          m_score->metaTag(u"copyright").toStdString().c_str());
    }

    // Add page size information for full score rendering
    this->writeSurfaceSizeInches(meiHead);
  }

  return true;
}

/**
 * Write the MEI score.
 * First write the initial scoreDef, and then loop through pages and systems.
 * Layout information (sb and pb) is written only when the corresponding output
 * option is selected.
 */

bool MeiExporter::writeScore() {
  pugi::xml_node music = m_mei.append_child("music");
  m_currentNode = music.append_child("body");
  m_currentNode = m_currentNode.append_child("mdiv");
  m_currentNode = m_currentNode.append_child("score");

  this->writeScoreDef();

  m_currentNode = m_currentNode.append_child();
  libmei::Section meiSection;
  meiSection.Write(m_currentNode, this->getSectionXmlId());

  int measureN = 0;
  bool isFirst = true;
  bool wasPreviousIrregular = true;

  bool exportLayout = configuration()->meiExportLayout();
  bool pageBreak = false;
  bool lineBreak = false;
  // bool sectionBreak = false; // not implemented, but we should had additional
  // sections elements where m_hasSections is true
  const System *prevSystem = nullptr;
  for (const Page *page : m_score->pages()) {
    if (exportLayout) {
      if (prevSystem) {
        const Measure *lastMeasure = prevSystem->lastMeasure();
        this->writeSystemTrailer(lastMeasure);
        prevSystem = nullptr;
      }
      libmei::Pb pb;
      if (pageBreak) {
        pb.SetType(BREAK_TYPE);
      }
      pb.Write(m_currentNode.append_child());
    }
    lineBreak = false; // reset at the start of each page
    for (const System *system : page->systems()) {
      if (exportLayout) {
        if (prevSystem) {
          const Measure *lastMeasure = prevSystem->lastMeasure();
          this->writeSystemTrailer(lastMeasure);
        }
        libmei::Sb sb;
        if (lineBreak) {
          sb.SetType(BREAK_TYPE);
        }
        pugi::xml_node sbNode = m_currentNode.append_child();
        sb.Write(sbNode);

        // Append bounding box for the system in inches
        RectF sysBbox = system->pageBoundingRect();
        double tlx = sysBbox.x() / DPI;
        double brx = (sysBbox.x() + sysBbox.width()) / DPI;
        double tly = toBottomLeftInches(sysBbox.y());
        double bry = toBottomLeftInches(sysBbox.y() + sysBbox.height());
        sbNode.append_attribute("pm:top-left-x") =
            formatDecimalStr(tlx, 3).c_str();
        sbNode.append_attribute("pm:top-left-y") =
            formatDecimalStr(tly, 3).c_str();
        sbNode.append_attribute("pm:bottom-right-x") =
            formatDecimalStr(brx, 3).c_str();
        sbNode.append_attribute("pm:bottom-right-y") =
            formatDecimalStr(bry, 3).c_str();
      }
      for (const MeasureBase *mBase : system->measures()) {
        if (mBase->isMeasure()) {
          const Measure *measure = static_cast<const Measure *>(mBase);
          this->writeEnding(measure);
          this->writeMeasure(measure, measureN, isFirst, wasPreviousIrregular);
          this->writeEndingEnd(measure);
        }
        lineBreak = mBase->lineBreak();
        pageBreak = mBase->pageBreak();
        // sectionBreak = mBase->sectionBreak(); // see comment above
      }
      prevSystem = system;
    }
  }

  // non critical assert
  assert(this->isCurrentNode(libmei::Section()));
  m_currentNode = m_currentNode.parent();

  return true;
}

/**
 * Write the initial score definition.
 */

bool MeiExporter::writeScoreDef() {
  m_currentNode = m_currentNode.append_child("scoreDef");
  pugi::xml_node scoreDefRoot =
      m_currentNode; // Keep a handle to attach pm: vectors later

  // If we have a VBox on the first measure, assume it to be a title frame and
  // use it for the pgHead
  MeasureBase *mBase = m_score->measures()->first();
  if (mBase->isVBox()) {
    this->writePgHead(toVBox(mBase));
  }

  // Number of staffGrp closing at each staff
  std::vector<int> staffGrpEnds(m_score->staves().size(), 0);

  const Measure *measure = nullptr;
  for (MeasureBase *mBase2 = m_score->measures()->first(); mBase2 != nullptr;
       mBase2 = mBase2->next()) {
    if (!measure && mBase2->isMeasure()) {
      // the first actual measure we are going built the scoreDef from
      measure = static_cast<const Measure *>(mBase2);
    }
    // Also check here if we have multiple sections in the score
    if (mBase2->sectionBreak()) {
      m_hasSections = true;
      break;
    }
  }

  // Probably no music in the file (see vtest/scores/frametext.mscx)
  if (!measure) {
    // pop the scoreDef
    m_currentNode = m_currentNode.parent();
    return true;
  }

  m_currentNode = m_currentNode.append_child("staffGrp");

  for (Part *part : m_score->parts()) {
    if (!part || !part->show()) {
      continue;
    }
    // For parts with more than one staff, write the label in the staffGrp
    const size_t visibleStaffCount = std::count_if(
        part->staves().cbegin(), part->staves().cend(), shouldExportStaff);
    Part *staffGrpPart = (visibleStaffCount > 1) ? part : nullptr;
    // Otherwise write the label in the staffDef
    bool isStaffDefPart = visibleStaffCount == 1;
    for (Staff *staff : part->staves()) {
      if (!shouldExportStaff(staff)) {
        continue;
      }
      this->writeStaffGrpStart(staff, staffGrpEnds, staffGrpPart);
      this->writeStaffDef(staff, measure, part, isStaffDefPart);
      this->writeStaffGrpEnd(staff, staffGrpEnds);
      // We pass the part only for the first staff of the staffGrp, so set it to
      // null after
      staffGrpPart = nullptr;
    }
  }

  // Pop the staffGrp
  m_currentNode = m_currentNode.parent();

  // Attach top/bottom staff vectors for initial time/key signatures on the
  // scoreDef itself. We look at the very first measure's header segments.
  if (measure) {
    Fraction tick = measure->tick();
    // Find the top staff (idx == 0)
    Staff *topStaff = nullptr;
    for (Staff *st : m_score->staves()) {
      if (st && st->idx() == 0) {
        topStaff = st;
        break;
      }
    }
    if (topStaff) {
      track_idx_t topTrackStart = staff2track(topStaff->idx());

      // Time signature on top/bottom staff
      if (Segment *timeSigSeg =
              measure->findSegment(SegmentType::TimeSig, tick)) {
        const TimeSig *tsTop =
            static_cast<const TimeSig *>(timeSigSeg->element(topTrackStart));
        const TimeSig *tsBottom = static_cast<const TimeSig *>(
            timeSigSeg->element(topTrackStart + VOICES));
        if (tsTop) {
          PointF p = tsTop->pagePos();
          double xIn = p.x() / DPI;
          double yIn = toBottomLeftInches(p.y());
          std::string xy = formatDecimalStr(xIn, 3) + std::string(",") +
                           formatDecimalStr(yIn, 3);
          scoreDefRoot.append_attribute("pm:timesig-xy-top") = xy.c_str();
          // Back-compat alias
          scoreDefRoot.append_attribute("pm:timesig-xy") = xy.c_str();
        }
        if (tsBottom) {
          PointF p = tsBottom->pagePos();
          double xIn = p.x() / DPI;
          double yIn = toBottomLeftInches(p.y());
          std::string xy = formatDecimalStr(xIn, 3) + std::string(",") +
                           formatDecimalStr(yIn, 3);
          scoreDefRoot.append_attribute("pm:timesig-xy-bottom") = xy.c_str();
        }
      }
      // Key signature on top/bottom staff (skip C major at start like staffDef
      // logic)
      if (Segment *keySigSeg =
              measure->findSegment(SegmentType::KeySig, tick)) {
        const KeySig *ksTop =
            static_cast<const KeySig *>(keySigSeg->element(topTrackStart));
        const KeySig *ksBottom = static_cast<const KeySig *>(
            keySigSeg->element(topTrackStart + VOICES));
        if (ksTop && ksTop->key() != Key::C) {
          PointF p = ksTop->pagePos();
          double xIn = p.x() / DPI;
          double yIn = toBottomLeftInches(p.y());
          std::string xy = formatDecimalStr(xIn, 3) + std::string(",") +
                           formatDecimalStr(yIn, 3);
          scoreDefRoot.append_attribute("pm:keysig-xy-top") = xy.c_str();
          // Back-compat alias
          scoreDefRoot.append_attribute("pm:keysig-xy") = xy.c_str();
        }
        if (ksBottom && ksBottom->key() != Key::C) {
          PointF p = ksBottom->pagePos();
          double xIn = p.x() / DPI;
          double yIn = toBottomLeftInches(p.y());
          std::string xy = formatDecimalStr(xIn, 3) + std::string(",") +
                           formatDecimalStr(yIn, 3);
          scoreDefRoot.append_attribute("pm:keysig-xy-bottom") = xy.c_str();
        }
      }
    }
  }

  // Pop the scoreDef
  m_currentNode = m_currentNode.parent();

  return true;
}

/**
 * Write the page header.
 * Uses the first MuseScore vBox.
 */

bool MeiExporter::writePgHead(const VBox *vBox) {
  IF_ASSERT_FAILED(vBox) { return false; }

  m_currentNode = m_currentNode.append_child();

  libmei::PgHead pgHead;
  pgHead.Write(m_currentNode);

  std::list<std::tuple<libmei::Rend, String, const Text *>> cells[CellCount];

  // For each text in the frame create an MEI Rend
  // Convert::textToMEI set the size of the Rend looking at the TextStyleType
  // It also places them (cell) accordingly
  for (const EngravingItem *element : vBox->el()) {
    if (element->isText()) {
      const Text *text = toText(element);
      auto [meiRend, cell, rendText] = Convert::textToMEI(text);
      cells[cell].push_back(std::make_tuple(meiRend, rendText, text));
    }
  }

  // Each cell is now a list of pairs of Rend and the corresponding text content
  // The text content is plain text but can be multiple lines separated with a
  // "\n"
  for (int cell = TopLeft; cell < CellCount; cell++) {
    if (cells[cell].empty()) {
      continue;
    }
    // if the cell is not empty, create a cell Rend an place it accordingly
    pugi::xml_node cellNode = m_currentNode.append_child();
    libmei::Rend meiRendCell;
    if (cell < 3) {
      meiRendCell.SetValign(libmei::VERTICALALIGNMENT_top);
    } else if (cell < 6) {
      meiRendCell.SetValign(libmei::VERTICALALIGNMENT_middle);
    } else {
      meiRendCell.SetValign(libmei::VERTICALALIGNMENT_bottom);
    }
    if ((cell % 3) == 0) {
      meiRendCell.SetHalign(libmei::HORIZONTALALIGNMENT_left);
    } else if ((cell % 3) == 1) {
      meiRendCell.SetHalign(libmei::HORIZONTALALIGNMENT_center);
    } else {
      meiRendCell.SetHalign(libmei::HORIZONTALALIGNMENT_right);
    }
    meiRendCell.Write(cellNode);
    // In the cell, write each Rend
    bool isFirst = true;
    for (auto &cellEntry : cells[cell]) {
      auto &[rend, rendText, sourceText] = cellEntry;
      // If we have more than one Rend in the cell-list, add an <lb/>
      if (!isFirst) {
        cellNode.append_child("lb");
      }
      pugi::xml_node rendNode = cellNode.append_child();
      rend.Write(rendNode);
      appendCenteredPmPosition(rendNode, sourceText);
      // Each Rend (as plain text) can itself be multi-line, split it with <lb/>
      StringList lines = rendText.split(u"\n");
      this->writeLines(rendNode, lines);
      isFirst = false;
    }
  }

  m_currentNode = m_currentNode.parent();

  return true;
}

/**
 * Write a list of string as lines separated with line breaks
 */

bool MeiExporter::writeLines(pugi::xml_node node, const StringList &lines) {
  if (lines.size() > 0) {
    node.text().set(lines[0].toStdString().c_str());
  }
  for (size_t index = 1; index < lines.size(); index++) {
    node.append_child("lb");
    pugi::xml_node textNode = node.append_child(pugi::node_pcdata);
    textNode.text() = lines[index].toStdString().c_str();
  }
  return true;
}

/**
 * Write a list of string as lines separated with line breaks.
 * For each line group the SMuFL symbols into <rend> with a @glyph.num
 * Convert line by line MuseScore plain text (without <sym>) into text segmented
 * text blocks
 */

bool MeiExporter::writeLinesWithSMuFL(pugi::xml_node node,
                                      const StringList &lines) {
  bool isFirst = true;
  for (size_t index = 0; index < lines.size(); index++) {
    if (!isFirst) {
      node.append_child("lb");
    }

    Convert::textWithSmufl lineBlocks;
    Convert::textToMEI(lineBlocks, lines.at(index));

    bool isFirstBlock = true;
    for (auto &block : lineBlocks) {
      if (block.first) {
        pugi::xml_node rendText = node.append_child();
        libmei::Rend meiRend;
        meiRend.SetGlyphAuth(SMUFL_AUTH);
        meiRend.Write(rendText);
        rendText.text() = block.second.toStdString().c_str();
      } else {
        if (isFirst && isFirstBlock) {
          node.text().set(block.second.toStdString().c_str());
        } else {
          pugi::xml_node textNode = node.append_child(pugi::node_pcdata);
          textNode.text() = block.second.toStdString().c_str();
        }
      }
      isFirstBlock = false;
    }
    isFirst = false;
  }
  return true;
}

/**
 * Write a score definition change.
 * The scoreDef is preprended to the m_currentNode that currently holds the
 * measure. The changes are encoded in the scoreDef element if that is possible.
 * Otherwise, staffGrp with staffDef are encoded (e.g., when changing a key
 * signature with transposing instruments)
 */

bool MeiExporter::writeScoreDefChange() {
  if (!m_timeSig && !m_keySig) {
    return true;
  }

  // First check that the timesig change is the same at all staves
  const TimeSig *scoreDefTimeSig = nullptr;
  if (m_timeSig) {
    for (size_t staff = 0; staff < m_score->nstaves(); staff++) {
      if (!shouldExportStaff(m_score->staff(staff))) {
        continue;
      }
      const TimeSig *current =
          dynamic_cast<const TimeSig *>(m_timeSig->element(staff2track(staff)));
      if (scoreDefTimeSig) {
        // Compare the current and the previous one, stop if they are different
        if (!current || (*current) != (*scoreDefTimeSig)) {
          scoreDefTimeSig = nullptr;
          break;
        }
      }
      scoreDefTimeSig = current;
      // The first staff had none, that will be different than any other one in
      // the segment anyway
      if (!scoreDefTimeSig) {
        break;
      }
    }
  }

  // Same for keysig changes (likely to be different with transposing
  // instruments)
  const KeySig *scoreDefKeySig = nullptr;
  if (m_keySig) {
    for (size_t staff = 0; staff < m_score->nstaves(); staff++) {
      if (!shouldExportStaff(m_score->staff(staff))) {
        continue;
      }
      const KeySig *current =
          dynamic_cast<const KeySig *>(m_keySig->element(staff2track(staff)));
      if (scoreDefKeySig) {
        if (!current || (current->key() != scoreDefKeySig->key())) {
          scoreDefKeySig = nullptr;
          break;
        }
      }
      scoreDefKeySig = current;
      // The first staff had none, same as for time sig
      if (!scoreDefKeySig) {
        break;
      }
    }
  }

  // m_currentNode is the last measure and we need to prepend the scoreDef
  pugi::xml_node scoreDefNode =
      m_currentNode.parent().insert_child_before("scoreDef", m_currentNode);
  libmei::ScoreDef meiScoreDef;

  // Prepare Pianomania coordinate vectors for system-level (scoreDef) changes.
  // Attach both top and bottom staff positions when available.
  std::string pmTimeXYTop;
  std::string pmTimeXYBottom;
  std::string pmKeyXYTop;
  std::string pmKeyXYBottom;

  // Single timesig change
  if (scoreDefTimeSig) {
    libmei::StaffDef timeSigDef = Convert::meterToMEI(
        scoreDefTimeSig->sig(), scoreDefTimeSig->timeSigType());
    meiScoreDef.SetMeterSym(timeSigDef.GetMeterSym());
    meiScoreDef.SetMeterUnit(timeSigDef.GetMeterUnit());
    meiScoreDef.SetMeterCount(timeSigDef.GetMeterCount());

    // Coordinates (inches) for the time signature change.
    const double timeXIn = m_timeSig ? m_timeSig->pagePos().x() / DPI : 0.0;
    const TimeSig *tsTop = dynamic_cast<const TimeSig *>(
        m_timeSig ? m_timeSig->element(staff2track(0)) : nullptr);
    const TimeSig *tsBottom = dynamic_cast<const TimeSig *>(
        m_timeSig ? m_timeSig->element(staff2track(1)) : nullptr);
    if (tsTop) {
      PointF p = tsTop->pagePos();
      double yIn = toBottomLeftInches(p.y());
      pmTimeXYTop = formatDecimalStr(timeXIn, 3) + std::string(",") +
                    formatDecimalStr(yIn, 3);
    }
    if (tsBottom) {
      PointF p = tsBottom->pagePos();
      double yIn = toBottomLeftInches(p.y());
      pmTimeXYBottom = formatDecimalStr(timeXIn, 3) + std::string(",") +
                       formatDecimalStr(yIn, 3);
    }
    // Standard x-position attribute for the score-def level change
    scoreDefNode.append_attribute("timeSig-x") =
        formatDecimalStr(timeXIn, 3).c_str();
  }
  // Single keysig change
  if (scoreDefKeySig) {
    // libmei::StaffDef keySigDef = Convert::keyToMEI(scoreDefKeySig->sig());
    meiScoreDef.SetKeysig(Convert::keyToMEI(scoreDefKeySig->key()));

    // Coordinates (inches) for the key signature change on the top staff
    const KeySig *ksTop = dynamic_cast<const KeySig *>(
        m_keySig ? m_keySig->element(staff2track(0)) : nullptr);
    const KeySig *ksBottom = dynamic_cast<const KeySig *>(
        m_keySig ? m_keySig->element(staff2track(1)) : nullptr);
    if (ksTop) {
      PointF p = ksTop->pagePos();
      double xIn = p.x() / DPI;
      double yIn = toBottomLeftInches(p.y());
      pmKeyXYTop = formatDecimalStr(xIn, 3) + std::string(",") +
                   formatDecimalStr(yIn, 3);
    }
    if (ksBottom) {
      PointF p = ksBottom->pagePos();
      double xIn = p.x() / DPI;
      double yIn = toBottomLeftInches(p.y());
      pmKeyXYBottom = formatDecimalStr(xIn, 3) + std::string(",") +
                      formatDecimalStr(yIn, 3);
    }
  }
  // Otherwise, add staffGrp/staffDef
  if ((!scoreDefTimeSig && m_timeSig) || (!scoreDefKeySig && m_keySig)) {
    pugi::xml_node staffGrpNode = scoreDefNode.append_child("staffGrp");
    for (size_t staff = 0; staff < m_score->nstaves(); staff++) {
      if (!shouldExportStaff(m_score->staff(staff))) {
        continue;
      }
      pugi::xml_node staffDefNode = staffGrpNode.append_child();
      libmei::StaffDef meiStaffDef;
      std::string staffTimeXY;
      std::string staffKeyXY;
      if (!scoreDefTimeSig && m_timeSig) {
        const TimeSig *timeSig = dynamic_cast<const TimeSig *>(
            m_timeSig->element(staff2track(staff)));
        if (timeSig) {
          meiStaffDef =
              Convert::meterToMEI(timeSig->sig(), timeSig->timeSigType());

          // Pianomania: per-staff time signature position (inches)
          double xIn = m_timeSig->pagePos().x() / DPI;
          PointF p = timeSig->pagePos();
          double yIn = toBottomLeftInches(p.y());
          staffTimeXY = formatDecimalStr(xIn, 3) + std::string(",") +
                        formatDecimalStr(yIn, 3);
        }
      }
      if (!scoreDefKeySig && m_keySig) {
        const KeySig *keySig =
            dynamic_cast<const KeySig *>(m_keySig->element(staff2track(staff)));
        if (keySig) {
          meiStaffDef.SetKeysig(Convert::keyToMEI(keySig->key()));

          // Pianomania: per-staff key signature position (inches)
          PointF p = keySig->pagePos();
          double xIn = p.x() / DPI;
          double yIn = toBottomLeftInches(p.y());
          staffKeyXY = formatDecimalStr(xIn, 3) + std::string(",") +
                       formatDecimalStr(yIn, 3);
        }
      }
      meiStaffDef.SetN(static_cast<int>(staff + 1));
      meiStaffDef.Write(staffDefNode);

      // Attach exported positions as Pianomania vectors on staffDef
      if (!staffTimeXY.empty()) {
        staffDefNode.append_attribute("pm:timesig-xy") = staffTimeXY.c_str();
      }
      if (!staffKeyXY.empty()) {
        staffDefNode.append_attribute("pm:keysig-xy") = staffKeyXY.c_str();
      }
    }
  }

  meiScoreDef.Write(scoreDefNode);

  // Attach exported positions as Pianomania vectors on scoreDef
  if (!pmTimeXYTop.empty()) {
    scoreDefNode.append_attribute("pm:timesig-xy-top") = pmTimeXYTop.c_str();
    // Back-compat alias
    scoreDefNode.append_attribute("pm:timesig-xy") = pmTimeXYTop.c_str();
  }
  if (!pmTimeXYBottom.empty()) {
    scoreDefNode.append_attribute("pm:timesig-xy-bottom") =
        pmTimeXYBottom.c_str();
  }
  if (!pmKeyXYTop.empty()) {
    scoreDefNode.append_attribute("pm:keysig-xy-top") = pmKeyXYTop.c_str();
    // Back-compat alias
    scoreDefNode.append_attribute("pm:keysig-xy") = pmKeyXYTop.c_str();
  }
  if (!pmKeyXYBottom.empty()) {
    scoreDefNode.append_attribute("pm:keysig-xy-bottom") =
        pmKeyXYBottom.c_str();
  }

  return true;
}

/**
 * Write a staffGrp (opening).
 * Increments in ends the ending position of the staffGrp to be closed by
 * MeiExporter::writeStaffGrpEnd.
 */

bool MeiExporter::writeStaffGrpStart(const Staff *staff, std::vector<int> &ends,
                                     const Part *staffGrpPart) {
  IF_ASSERT_FAILED(staff) { return false; }

  for (size_t j = 0; j < staff->bracketLevels() + 1; j++) {
    if (staff->bracketType(j) != BracketType::NO_BRACKET) {
      libmei::StaffGrp meiStaffGrp =
          Convert::staffGrpToMEI(staff->bracketType(j), staff->barLineSpan());
      // mark at which staff we will need to close the staffGrp
      int end = static_cast<int>(staff->idx() + staff->bracketSpan(j)) - 1;
      // Something is wrong, maybe a staff was delete in the MuseScore file?
      if (end >= static_cast<int>(ends.size())) {
        continue;
      }
      ends.at(end)++;
      //
      m_currentNode = m_currentNode.append_child();
      meiStaffGrp.Write(m_currentNode);
      // If we have a part and reached the latest level, write the label and
      // labelAbbr
      if (staffGrpPart && j == staff->bracketLevels()) {
        this->writeLabel(m_currentNode, staffGrpPart);
        this->writeInstrDef(m_currentNode, staffGrpPart);
      }
    }
  }
  return true;
}

/**
 * Write a staffGrp (closing).
 * Looks in ends how many staffGrp levels need to be closed for the
 * corresponding staffIdx.
 */

bool MeiExporter::writeStaffGrpEnd(const Staff *staff, std::vector<int> &ends) {
  IF_ASSERT_FAILED(staff) { return false; }

  size_t idx = staff->idx();
  for (int i = 0; i < ends.at(idx); i++) {
    m_currentNode = m_currentNode.parent();
  }

  return true;
}

/**
 * Write the initial staff definitions.
 */

bool MeiExporter::writeStaffDef(const Staff *staff, const Measure *measure,
                                const Part *part, bool isPart) {
  IF_ASSERT_FAILED(staff && measure && part) { return false; }

  libmei::StaffDef meiStaffDef = Convert::staffToMEI(staff);
  pugi::xml_node staffDefNode = m_currentNode.append_child();

  // Pianomania coordinate vectors for initial staffDef context (inches)
  std::string pmTimeXY;
  std::string pmKeyXY;

  if (isPart) {
    this->writeLabel(staffDefNode, part);
    this->writeInstrDef(staffDefNode, part);
  }

  if (measure) {
    Fraction tick = measure->tick();

    track_idx_t startTrack = staff2track(staff->idx());
    track_idx_t endTrack = startTrack + VOICES;

    // clef
    Segment *clefSeg = measure->findSegment(SegmentType::HeaderClef, tick);
    if (clefSeg) {
      for (track_idx_t track = startTrack; track < endTrack; ++track) {
        Clef *clef = static_cast<Clef *>(clefSeg->element(track));
        if (clef) {
          libmei::Clef meiClef = Convert::clefToMEI(clef->clefType());
          Convert::colorToMEI(clef, meiClef);
          pugi::xml_node clefNode = staffDefNode.append_child();
          meiClef.Write(clefNode);
          break;
        }
      }
    }
    // time signature
    Segment *timeSigSeg = measure->findSegment(SegmentType::TimeSig, tick);
    if (timeSigSeg) {
      for (track_idx_t track = startTrack; track < endTrack; ++track) {
        TimeSig *timeSig = static_cast<TimeSig *>(timeSigSeg->element(track));
        if (timeSig) {
          libmei::StaffDef timeSigDef =
              Convert::meterToMEI(timeSig->sig(), timeSig->timeSigType());
          meiStaffDef.SetMeterSym(timeSigDef.GetMeterSym());
          meiStaffDef.SetMeterUnit(timeSigDef.GetMeterUnit());
          meiStaffDef.SetMeterCount(timeSigDef.GetMeterCount());

          // Pianomania: export the time signature anchor position (inches)
          PointF p = timeSig->pagePos();
          double xIn = p.x() / DPI;
          double yIn = toBottomLeftInches(p.y());
          pmTimeXY = formatDecimalStr(xIn, 3) + std::string(",") +
                     formatDecimalStr(yIn, 3);
          break;
        }
      }
    }
    // key signature
    Segment *keySigSeg = measure->findSegment(SegmentType::KeySig, tick);
    if (keySigSeg) {
      for (track_idx_t track = startTrack; track < endTrack; ++track) {
        KeySig *keySig = static_cast<KeySig *>(keySigSeg->element(track));
        // For the initial staffDef we do not write @key.sig="0"
        if (keySig && keySig->key() != Key::C) {
          meiStaffDef.SetKeysig(Convert::keyToMEI(keySig->key()));

          // Pianomania: export the key signature anchor position (inches)
          PointF p = keySig->pagePos();
          double xIn = p.x() / DPI;
          double yIn = toBottomLeftInches(p.y());
          pmKeyXY = formatDecimalStr(xIn, 3) + std::string(",") +
                    formatDecimalStr(yIn, 3);
          break;
        }
      }
    }
  }

  meiStaffDef.Write(staffDefNode);

  // Attach exported positions as Pianomania vectors on staffDef
  // Only add if we actually found a symbol in this context.
  if (!pmTimeXY.empty()) {
    staffDefNode.append_attribute("pm:timesig-xy") = pmTimeXY.c_str();
  }
  if (!pmKeyXY.empty()) {
    staffDefNode.append_attribute("pm:keysig-xy") = pmKeyXY.c_str();
  }

  return true;
}

/**
 * Write label and label abbreviations.
 */

bool MeiExporter::writeLabel(pugi::xml_node node, const Part *part) {
  IF_ASSERT_FAILED(part) { return false; }

  StringList lines;
  const Instrument *instrument = part->instrument();
  if (instrument && instrument->longNames().size() > 0) {
    libmei::Label meiLabel;
    pugi::xml_node labelNode = node.append_child();
    meiLabel.Write(labelNode);
    lines = instrument->nameAsPlainText().split(u"\n");
    this->writeLines(labelNode, lines);
  }
  if (instrument && instrument->shortNames().size() > 0) {
    libmei::LabelAbbr meiLabelAbbr;
    pugi::xml_node labelAbbrNode = node.append_child();
    meiLabelAbbr.Write(labelAbbrNode);
    lines = instrument->abbreviatureAsPlainText().split(u"\n");
    this->writeLines(labelAbbrNode, lines);
  }

  return true;
}

/**
 * Write instrument definition for MIDI information.
 */

bool MeiExporter::writeInstrDef(pugi::xml_node node, const Part *part) {
  IF_ASSERT_FAILED(part) { return false; }

  const int midiProgram = part->midiProgram();
  // const int midiChannel = part->midiChannel();
  // const int midiPort = part->midiPort();

  if (midiProgram < 0) {
    return false;
  }

  libmei::InstrDef meiInstrDef;
  pugi::xml_node instrDefNode = node.append_child();
  if (midiProgram >= 0 && midiProgram < 128) {
    meiInstrDef.SetMidiInstrnum(midiProgram);
  }
  meiInstrDef.Write(instrDefNode);

  return true;
}

/**
 * Write an ending (opening).
 * Performs a lookup of voltas spanning the measure with
 * MeiExporter::findVoltasInMeasure
 */

bool MeiExporter::writeEnding(const Measure *measure) {
  std::list<const Volta *> voltas = this->findVoltasInMeasure(measure);
  auto voltaIter =
      std::find_if(voltas.begin(), voltas.end(), [measure](const Volta *volta) {
        return volta->startMeasure() == measure;
      });

  if (voltaIter != voltas.end()) {
    const Volta *volta = *voltaIter;
    libmei::Ending meiEnding = Convert::endingToMEI(volta);
    m_currentNode = m_currentNode.append_child();
    meiEnding.Write(m_currentNode, this->getXmlIdFor(volta, 'e'));
    appendPmLineEndpoints(m_currentNode, volta, "pm:x1y1x2y2");

    const VoltaSegment *segment = toVoltaSegment(volta->frontSegment());

    if (segment) {
      const double lineYOffset = getVoltaLineYOffset(segment);
      m_currentNode.append_attribute("pm:voltaLineYOffset") =
          formatDecimalStr(lineYOffset, 3).c_str();

      if (segment->text()) {
        const double labelYOffset = getVoltaLabelYOffset(segment);
        m_currentNode.append_attribute("pm:voltaLabelYOffset") =
            formatDecimalStr(labelYOffset, 3).c_str();
      }
    }
  }
  return true;
}

/**
 * Write an ending (closing).
 * Performs a lookup of voltas spanning the measure with
 * MeiExporter::findVoltasInMeasure
 */

bool MeiExporter::writeEndingEnd(const Measure *measure) {
  std::list<const Volta *> voltas = this->findVoltasInMeasure(measure);
  auto voltaIter =
      std::find_if(voltas.begin(), voltas.end(), [measure](const Volta *volta) {
        return volta->endMeasure() == measure;
      });

  if (voltaIter != voltas.end()) {
    // non critical assert
    assert(this->isCurrentNode(libmei::Ending()));
    m_currentNode = m_currentNode.parent();
  }
  return true;
}

bool MeiExporter::writeSystemTrailer(const Measure *measure) {
  if (!measure) {
    return false;
  }

  bool hasDoubleBar = false;
  bool hasKeySig = false;
  bool hasTimeSig = false;
  double doubleBarX = 0.0;
  // Track any visible end barline position (single or double)
  bool hasAnyBarline = false;
  double barlineX = 0.0;
  double keyX = 0.0;
  double timeX = 0.0;
  // Values to serialize
  std::string keySigVal; // e.g., "4s", "3f", "0"
  enum class MeterKind { NONE, NUMERIC, COMMON, CUT };
  MeterKind meterKind = MeterKind::NONE;
  int meterCount = 0;
  int meterUnit = 0;

  if (measure->endBarLineVisible()) {
    const BarLine *bl = measure->endBarLine();
    if (bl) {
      // Capture generic barline position
      hasAnyBarline = true;
      barlineX = bl->pagePos().x() / DPI;
      // Additionally check for explicit double barline
      if (measure->endBarLineType() == BarLineType::DOUBLE) {
        hasDoubleBar = true;
        doubleBarX = barlineX;
      }
    }
  }

  Segment *keySeg =
      measure->findSegmentR(SegmentType::KeySigAnnounce, measure->ticks());
  if (keySeg) {
    for (track_idx_t track = 0; track < m_score->nstaves() * VOICES;
         track += VOICES) {
      const KeySig *ks = toKeySig(keySeg->element(track));
      if (ks && ks->visible()) {
        hasKeySig = true;
        keyX = ks->pagePos().x() / DPI;
        // Compute keysig textual value: number of accidentals + 's'/'f'
        int acc = static_cast<int>(ks->key()); // -7..+7, 0 == C
        if (acc == 0) {
          keySigVal = "0";
        } else if (acc > 0) {
          keySigVal = std::to_string(acc) + "s"; // sharps
        } else { // acc < 0
          keySigVal = std::to_string(-acc) + "f"; // flats
        }
        break;
      }
    }
  }

  Segment *timeSeg =
      measure->findSegmentR(SegmentType::TimeSigAnnounce, measure->ticks());
  if (timeSeg) {
    for (track_idx_t track = 0; track < m_score->nstaves() * VOICES;
         track += VOICES) {
      const TimeSig *ts = toTimeSig(timeSeg->element(track));
      if (ts && ts->visible()) {
        hasTimeSig = true;
        timeX = ts->pagePos().x() / DPI;
        // Determine meter representation: symbolic common/cut or numeric
        switch (ts->timeSigType()) {
        case TimeSigType::FOUR_FOUR:
          meterKind = MeterKind::COMMON;
          break;
        case TimeSigType::ALLA_BREVE:
          meterKind = MeterKind::CUT;
          break;
        default:
          meterKind = MeterKind::NUMERIC;
          meterCount = ts->numerator();
          meterUnit = ts->denominator();
          break;
        }
        break;
      }
    }
  }

  if (!(hasDoubleBar || hasKeySig || hasTimeSig)) {
    return false;
  }

  pugi::xml_node stNode = m_currentNode.append_child("systemTrailer");
  if (hasDoubleBar) {
    stNode.append_attribute("doubleBarLine") = true;
    stNode.append_attribute("doubleBarLine-x") =
        formatDecimalStr(doubleBarX, 3).c_str();
  }
  if (hasKeySig) {
    // keysig textual value per request (e.g., "4s", "3f", "0")
    if (!keySigVal.empty()) {
      stNode.append_attribute("keysig") = keySigVal.c_str();
    }
    // keep x-position attribute name as-is
    stNode.append_attribute("keySig-x") = formatDecimalStr(keyX, 3).c_str();
  }
  if (hasTimeSig) {
    // Write MEI-like meter attributes
    switch (meterKind) {
    case MeterKind::COMMON:
      stNode.append_attribute("meter.sym") = "common";
      break;
    case MeterKind::CUT:
      stNode.append_attribute("meter.sym") = "cut";
      break;
    case MeterKind::NUMERIC:
      stNode.append_attribute("meter.count") = std::to_string(meterCount).c_str();
      stNode.append_attribute("meter.unit") = std::to_string(meterUnit).c_str();
      break;
    case MeterKind::NONE:
      break;
    }
    // keep x-position attribute name as-is
    stNode.append_attribute("timeSig-x") = formatDecimalStr(timeX, 3).c_str();
  }

  // If a time/key signature was announced at the system end, include the
  // barline x-position as a reference for layout consumers.
  if ((hasKeySig || hasTimeSig) && hasAnyBarline) {
    stNode.append_attribute("barline-x") =
        formatDecimalStr(barlineX, 3).c_str();
  }

  return true;
}

/**
 * Collect all note events in a measure and store them with both their
 * measure-relative tick position (integer) and their human-readable
 * timestamp (double).
 *
 * - The tick is used as the grouping key to guarantee that simultaneous
 *   notes across different staves and voices are treated as occurring
 *   at the same time.
 * - The timestamp is still available for conversion to beats if needed
 *   elsewhere (for example, for serialization).
 */
bool MeiExporter::collectNoteEvents(const Measure *measure) {
  // Clear any previously stored note events from an earlier measure.
  m_durEvents.clear();

  // Iterate through all staves in the score.
  for (Staff *staff : m_score->staves()) {
    if (!shouldExportStaff(staff)) {
      continue;
    }
    // Each staff has several voices, mapped to tracks.
    track_idx_t startTrackIndex = staff2track(staff->idx());
    track_idx_t endTrackIndex = startTrackIndex + VOICES;

    // Iterate through each voice (track) of this staff.
    for (track_idx_t track = startTrackIndex; track < endTrackIndex; ++track) {
      // Iterate through all segments in this measure.
      for (Segment *segment = measure->first(); segment;
           segment = segment->next()) {
        // Get the engraving item at the current track.
        const EngravingItem *engravingItem = segment->element(track);

        // Skip if there is nothing here or if the item was generated.
        if (!engravingItem || engravingItem->generated())
          continue;

        // Compute timing information for this segment:
        // - rel is the relative tick offset from the start of the measure.
        // - tickKey is an integer suitable for grouping simultaneous notes.
        // - t is the fractional timestamp, useful for beat calculations.
        Fraction rel = segment->tick() - measure->tick();
        int tickKey = rel.ticks();
        double t = Convert::tstampFromFraction(rel, measure->timesig());

        // Helper lambda to add all notes from a chord to m_durEvents.
        auto pushChordNotes = [&](const Chord *chord,
                                  const Chord *playbackParent) {
          std::vector<const Note *> notesToExport = visibleNotes(chord);
          if (notesToExport.empty()) {
            return true;
          }

          const Chord *indexGroupChord = chord->isGrace() ? chord : nullptr;
          for (const Note *note : notesToExport) {
            if (note->tieBack()) {
              // A tied continuation is written notation, but it correctly has
              // no new audible attack. Keep its written tick so the existing
              // tie-alias assignment can bind it to the tie start.
              m_durEvents.push_back({tickKey, t, note, indexGroupChord});
              continue;
            }
            const NoteEventList &events = note->playEvents();
            const int writtenAttackIndex =
                CompatMidiRendererInternal::canonicalWrittenNoteEventIndex(events);
            if (writtenAttackIndex < 0
                || events[writtenAttackIndex].pitch() != 0) {
              if (m_requirePrecomputedPianomaniaIndices) {
                LOGE() << "Coordinated Pianomania MEI export is missing a "
                          "canonical written playback attack";
                return false;
              }
              m_durEvents.push_back({tickKey, t, note, indexGroupChord});
              continue;
            }
            const int effectiveTick = tickKey
                + (playbackParent->actualTicks().ticks()
                   * events[writtenAttackIndex].ontime())
                      / NoteEvent::NOTE_LENGTH;
            m_durEvents.push_back({effectiveTick, t, note, indexGroupChord});
          }
          return true;
        };

        if (engravingItem->isChord()) {
          // This is a chord. Add its notes.
          const Chord *chord = toChord(engravingItem);

          // Include written grace notes in playback/index order so same-pitch
          // grace notes do not alias the anchor note.
          for (const Chord *graceChord : chord->graceNotesBefore()) {
            if (!pushChordNotes(graceChord, chord)) {
              return false;
            }
          }

          if (!pushChordNotes(chord, chord)) {
            return false;
          }

          for (const Chord *graceChord : chord->graceNotesAfter()) {
            if (!pushChordNotes(graceChord, chord)) {
              return false;
            }
          }
        } else if (engravingItem->isGraceNotesGroup()) {
          // This is a standalone grace notes group, not attached to a chord.
          const GraceNotesGroup *graceGroup = toGraceNotesGroup(engravingItem);
          for (const Chord *graceChord : *graceGroup) {
            if (!pushChordNotes(graceChord, graceChord)) {
              return false;
            }
          }
        }
      }
    }
  }
  return true;
}

/**
 * Sort and group notes by time and pitch for each measure, then assign indices
 * based on the sorted order.
 */
bool MeiExporter::assignIndicesForMeasure(const Measure *measure) {
  if (!collectNoteEvents(measure)) {
    return false;
  }

  // deterministic order: by tick, then by pitch
  std::stable_sort(m_durEvents.begin(), m_durEvents.end(),
                   [](const DurEvent &a, const DurEvent &b) {
                     if (a.tick != b.tick)
                       return a.tick < b.tick;
                     return a.note->ppitch() < b.note->ppitch();
                   });

  m_noteIdxAssignment.clear(); // per-measure cache is fine

  // helper: find index for a continuation note (tieBack != nullptr)
  auto findTieIndex = [&](const Note *n, int p) -> int {
    const Note *initial = n;
    while (initial->tieBack())
      initial = initial->tieBack()->startNote();

    // prefer same-measure cache if the start is earlier in this measure
    if (auto it = m_noteIdxAssignment.find(initial);
        it != m_noteIdxAssignment.end())
      return it->second;

    // otherwise, look in the global tie-start map (from earlier measures)
    if (auto it = m_idxForTieStart.find(initial); it != m_idxForTieStart.end())
      return it->second;

    // defensive fallback: shouldn’t happen if measures are processed in order
    int idx = updateNoteIndex(p);
    m_idxForTieStart[initial] = idx;
    return idx;
  };

  // walk groups of equal tick
  for (size_t i = 0; i < m_durEvents.size();) {
    int tick = m_durEvents[i].tick;
    size_t j = i;
    while (j < m_durEvents.size() && m_durEvents[j].tick == tick)
      ++j;

    struct IndexGroupKey {
      int pitch = 0;
      const Chord *chord = nullptr;

      bool operator==(const IndexGroupKey &other) const {
        return pitch == other.pitch && chord == other.chord;
      }
    };

    struct IndexGroupKeyHash {
      size_t operator()(const IndexGroupKey &key) const {
        return std::hash<int>()(key.pitch) ^
               (std::hash<const Chord *>()(key.chord) << 1);
      }
    };

    // Ensure one fresh index per (tick, pitch, grace chord) for untied notes.
    // Normal same-beat same-pitch notes still share an index, but written grace
    // notes get their own index because the MIDI export emits them as real
    // note events.
    std::unordered_map<IndexGroupKey, int, IndexGroupKeyHash> idxByPitch;

    for (size_t k = i; k < j; ++k) {
      const Note *note = m_durEvents[k].note;
      int p = note->ppitch();
      int idx;

      if (note->tieBack() != nullptr) {
        idx = findTieIndex(note, p);
      } else {
        IndexGroupKey key { p, m_durEvents[k].indexGroupChord };
        auto it = idxByPitch.find(key);
        if (it != idxByPitch.end()) {
          idx = it->second; // same-beat same-pitch across staves
        } else {
          idx = updateNoteIndex(p);
          idxByPitch.emplace(key, idx);
        }

        // if this note starts a tie forward, remember it globally
        if (note->tieFor() != nullptr) {
          m_idxForTieStart.emplace(note, idx);
        }
      }

      m_noteIdxAssignment[note] = idx;
    }

    i = j; // next tick group
  }
  return true;
}

/**
 * Write a measure and its content.
 * Write the staves and the control events.
 * Prepends a scoreDef change if a changing key signature or time signature is
 * encountered in the content.
 */

bool MeiExporter::writeMeasure(const Measure *measure, int &measureN,
                               bool &isFirst, bool &wasPreviousIrregular) {
  IF_ASSERT_FAILED(measure) { return false; }

  bool success = true;

  libmei::Measure meiMeasure =
      Convert::measureToMEI(measure, measureN, wasPreviousIrregular);
  m_currentNode = m_currentNode.append_child();
  meiMeasure.Write(m_currentNode, this->getMeasureXmlId(measure));

  // Append Pianomania bounding box attributes (in inches)
  RectF measureBbox = measure->pageBoundingRect();
  double tlx = measureBbox.x() / DPI;
  double brx = (measureBbox.x() + measureBbox.width()) / DPI;
  double tly = toBottomLeftInches(measureBbox.y());
  double bry =
      toBottomLeftInches(measureBbox.y() + measureBbox.height());
  m_currentNode.append_attribute("pm:top-left-x") =
      formatDecimalStr(tlx, 3).c_str();
  m_currentNode.append_attribute("pm:top-left-y") =
      formatDecimalStr(tly, 3).c_str();
  m_currentNode.append_attribute("pm:bottom-right-x") =
      formatDecimalStr(brx, 3).c_str();
  m_currentNode.append_attribute("pm:bottom-right-y") =
      formatDecimalStr(bry, 3).c_str();

  const BarLine *startRepeatBarline = firstBarlineInSegment(
      measure->findSegmentR(SegmentType::StartRepeatBarLine, Fraction(0, 1)));
  if (measure->repeatStart() && startRepeatBarline) {
    const double startRepeatX = startRepeatBarline->pageBoundingRect().x() / DPI;
    m_currentNode.append_attribute("pm:left-repeat-x") =
        formatDecimalStr(startRepeatX, 3).c_str();
  }

  const BarLine *endBarline = measure->endBarLine();
  const BarLineType endBarlineType = measure->endBarLineType();
  if (endBarline && (endBarlineType == BarLineType::END_REPEAT ||
                     endBarlineType == BarLineType::END_START_REPEAT)) {
    const double endRepeatX = endBarline->pageBoundingRect().x() / DPI;
    m_currentNode.append_attribute("pm:right-repeat-x") =
        formatDecimalStr(endRepeatX, 3).c_str();
  }

  // Reset keySig and timeSig change
  m_keySig = nullptr;
  m_timeSig = nullptr;

  // —————————————————————————————
  // 1) Build & sort all note‐events in this measure.
  if (!assignIndicesForMeasure(measure)) {
    return false;
  }
  // 2) Procede to write out staves/layers/notes using those precomputed
  // indices: —————————————————————————————

  for (Staff *staff : m_score->staves()) {
    if (!shouldExportStaff(staff)) {
      continue;
    }
    this->writeStaff(staff, measure);
  }

  for (EngravingItem *item : measure->el()) {
    switch (item->type()) {
    case ElementType::JUMP:
      success = success && this->writeRepeatMark(toJump(item), measure);
      break;
    case ElementType::MARKER:
      success = success && this->writeRepeatMark(toMarker(item), measure);
      break;
    default:
      break;
    }
  }

  for (auto controlEvent : m_startingControlEventList) {
    if (controlEvent.first->isArpeggio()) {
      success =
          success &&
          this->writeArpeg(dynamic_cast<const Arpeggio *>(controlEvent.first),
                           controlEvent.second);
    } else if (controlEvent.first->isBreath()) {
      success = success && this->writeBreath(
                               dynamic_cast<const Breath *>(controlEvent.first),
                               controlEvent.second);
    } else if (controlEvent.first->isExpression() ||
               controlEvent.first->isPlayTechAnnotation() ||
               controlEvent.first->isStaffText()) {
      success =
          success &&
          this->writeDir(dynamic_cast<const TextBase *>(controlEvent.first),
                         controlEvent.second);
    } else if (controlEvent.first->isDynamic()) {
      const Dynamic *dynamic =
          dynamic_cast<const Dynamic *>(controlEvent.first);
      success = success &&
                (dynamic->dynamicType() == DynamicType::OTHER
                     ? this->writeDir(dynamic, controlEvent.second)
                     : this->writeDynam(dynamic, controlEvent.second));
    } else if (controlEvent.first->isFermata()) {
      success =
          success &&
          this->writeFermata(dynamic_cast<const Fermata *>(controlEvent.first),
                             controlEvent.second);
    } else if (controlEvent.first->isFiguredBass()) {
      success =
          success &&
          this->writeFb(dynamic_cast<const FiguredBass *>(controlEvent.first),
                        controlEvent.second);
    } else if (controlEvent.first->isFingering()) {
      success =
          success &&
          this->writeFing(dynamic_cast<const Fingering *>(controlEvent.first),
                          controlEvent.second);
    } else if (controlEvent.first->isGlissando()) {
      success = success &&
                this->writeGliss(dynamic_cast<const Glissando *>(controlEvent.first),
                                 controlEvent.second);
    } else if (controlEvent.first->isHairpin()) {
      success =
          success &&
          this->writeHairpin(dynamic_cast<const Hairpin *>(controlEvent.first),
                             controlEvent.second);
    } else if (controlEvent.first->isHarmony()) {
      success =
          success &&
          this->writeHarm(dynamic_cast<const Harmony *>(controlEvent.first),
                          controlEvent.second);
    } else if (controlEvent.first->isHarpPedalDiagram()) {
      success = success &&
                this->writeHarpPedal(
                    dynamic_cast<const HarpPedalDiagram *>(controlEvent.first),
                    controlEvent.second);
    } else if (controlEvent.first->isOrnament()) {
      success = success && this->writeOrnament(dynamic_cast<const Ornament *>(
                                                   controlEvent.first),
                                               controlEvent.second);
    } else if (controlEvent.first->isOttava()) {
      success = success && this->writeOctave(
                               dynamic_cast<const Ottava *>(controlEvent.first),
                               controlEvent.second);
    } else if (controlEvent.first->isPedal()) {
      success = success && this->writePedal(
                               dynamic_cast<const Pedal *>(controlEvent.first),
                               controlEvent.second);
    } else if (controlEvent.first->isRubatoZone()) {
      success = success &&
                this->writeRubatoZone(
                    dynamic_cast<const RubatoZone *>(controlEvent.first),
                    controlEvent.second);
    } else if (controlEvent.first->isPyroSpan()) {
      success = success &&
                this->writeDanceShowSpan(
                    dynamic_cast<const Spanner *>(controlEvent.first),
                    controlEvent.second, "pm-pyro-span", 'y');
    } else if (controlEvent.first->isLaserSpan()) {
      success = success &&
                this->writeDanceShowSpan(
                    dynamic_cast<const Spanner *>(controlEvent.first),
                    controlEvent.second, "pm-laser-span", 'l');
    } else if (controlEvent.first->isRehearsalMark()) {
      success = success &&
                this->writeRehearsalMark(
                    dynamic_cast<const RehearsalMark *>(controlEvent.first),
                    controlEvent.second);
    } else if (controlEvent.first->isSlur()) {
      success = success &&
                this->writeSlur(dynamic_cast<const Slur *>(controlEvent.first),
                                controlEvent.second);
    } else if (controlEvent.first->isTempoText()) {
      success =
          success &&
          this->writeTempo(dynamic_cast<const TempoText *>(controlEvent.first),
                           controlEvent.second);
    } else if (controlEvent.first->isTie()) {
      success = success &&
                this->writeTie(dynamic_cast<const Tie *>(controlEvent.first),
                               controlEvent.second);
    } else if (controlEvent.first->isTrill()) {
      success = success && this->writeTrill(
                               dynamic_cast<const Trill *>(controlEvent.first),
                               controlEvent.second);
    }
  }
  m_startingControlEventList.clear();
  m_pendingControlEventList.clear();

  for (auto controlEvent : m_tstampControlEventMap) {
    if (controlEvent.first->isFermata()) {
      success =
          success && this->writeFermata(
                         dynamic_cast<const Fermata *>(controlEvent.first),
                         controlEvent.second.first, controlEvent.second.second);
    }
  }
  m_tstampControlEventMap.clear();

  this->addEndidToControlEvents();

  // This will prepend the scoreDef
  if (!isFirst) {
    this->writeScoreDefChange();
  }
  isFirst = false;

  m_currentNode = m_currentNode.parent();

  return success;
}

/**
 * Write a staff and its content.
 * Checks if each voice has some content.
 */

bool MeiExporter::writeStaff(const Staff *staff, const Measure *measure) {
  IF_ASSERT_FAILED(staff && measure) { return false; }

  m_currentNode = m_currentNode.append_child();

  libmei::Staff meiStaff;
  meiStaff.SetN(static_cast<int>(staff->idx() + 1));
  meiStaff.Write(m_currentNode, this->getStaffXmlId());

  track_idx_t startTrack = staff2track(staff->idx());
  track_idx_t endTrack = startTrack + VOICES;
  for (track_idx_t track = startTrack; track < endTrack; ++track) {
    writeLayer(track, staff, measure);
  }

  m_currentNode = m_currentNode.parent();

  return true;
}

/**
 * Write a layer (i.e., voice) and its content.
 */

bool MeiExporter::writeLayer(track_idx_t track, const Staff *staff,
                             const Measure *measure) {
  IF_ASSERT_FAILED(staff) { return false; }

  // If there is no voice, only increase the layer@n
  if (!measure->hasVoice(track)) {
    this->getLayerXmlId();
    return true;
  }

  m_currentNode = m_currentNode.append_child();
  libmei::Layer meiLayer;
  meiLayer.SetN(static_cast<int>(track2voice(track) + 1));
  meiLayer.Write(m_currentNode, this->getLayerXmlId());

  if (measure->measureRepeatNumMeasures(track2staff(track)) == 1) {
    MeasureRepeat *measureRepeat =
        measure->measureRepeatElement(track2staff(track));
    this->writeMRpt(measureRepeat);
    return true;
  }

  for (Segment *seg = measure->first(); seg; seg = seg->next()) {
    if (seg->segmentType() == SegmentType::EndBarLine) {
      this->addFermataToMap(track, seg, measure);
    }

    // Do not go any further than the measure tick (ignore EndBarLine,
    // KeySigAnnounce, TimeSigAnnounce)
    const EngravingItem *item = seg->element(track);
    if (!item || item->generated()) {
      continue;
    }

    if (item->isClef()) {
      // staff->idx() is zero-based; MEI staff @n is 1-based.
      this->writeClef(dynamic_cast<const Clef *>(item), measure, seg,
                      static_cast<int>(staff->idx() + 1));
    } else if (item->isChord()) {
      this->writeChord(dynamic_cast<const Chord *>(item), staff);
    } else if (item->isRest()) {
      this->writeRest(dynamic_cast<const Rest *>(item), staff);
    } else if (item->isBarLine()) {
      //
    } else if (item->isBreath()) {
      //
    } else if (item->isKeySig()) {
      if (m_keySig && (seg != m_keySig)) {
        LOGD() << "MeiExporter::writeLayer unexpected KeySig segment";
      }
      m_keySig = seg;
    } else if (item->isTimeSig()) {
      if (m_timeSig && (seg != m_timeSig)) {
        LOGD() << "MeiExporter::writeLayer unexpected TimeSig segment";
      }
      m_timeSig = seg;
    } else {
      LOGD() << "MeiExporter::writeLayer unknown segment type "
             << item->typeName();
    }
  }

  m_currentNode = m_currentNode.parent();

  return true;
}

//---------------------------------------------------------
// write MEI layer elements
//---------------------------------------------------------

/**
 * Write the artics attached to a Chord
 */

bool MeiExporter::writeArtics(const Chord *chord) {
  IF_ASSERT_FAILED(chord) { return false; }

  for (const Articulation *articulation : chord->articulations()) {
    if (articulation->isArticulation() &&
        !this->isLaissezVibrer(articulation->symId())) {
      this->writeArtic(articulation);
    }
  }

  return true;
}

/**
 * Write an artic (articulation).
 */

bool MeiExporter::writeArtic(const Articulation *articulation) {
  IF_ASSERT_FAILED(articulation) { return false; }

  libmei::Artic meiArtic = Convert::articToMEI(articulation);

  // Get the chord this articulation belongs to.
  const ChordRest *cr = articulation->chordRest();
  const bool articulationBelongsToChord = cr && cr->isChord();
  pugi::xml_node articNode = m_currentNode.append_child();
  meiArtic.Write(articNode, this->getXmlIdFor(articulation, 'a'));

  if (articulationBelongsToChord) {
    const Chord *chord = toChord(cr);
    // Calculate y-position relative to bottom note.
    double yPos = getArticulationYOffset(articulation, chord);

    // Add yOffset as a custom attribute using formatDecimalStr
    articNode.append_attribute("yOffset") = formatDecimalStr(yPos, 1);
  }

  PointF articPos = articulation->pagePos();
  double ax = articPos.x() / DPI;
  double ay = toBottomLeftInches(articPos.y());
  std::string articXY = formatDecimalStr(ax, 3) + std::string(",") +
                        formatDecimalStr(ay, 3);
  articNode.append_attribute("pm:xy") = articXY.c_str();

  return true;
}

/**
 * Open and close beam and tuplet elements for a ChordRest.
 * When both a beam and a tuplet is opening and closing, check which is the
 * appropriate nesting order. By default, nest the tuplet within the beam.
 * Closing beam and tuplet only change the bool parameters and actually closing
 * the elements is happening in MeiExporter::writeBeamAndTupletEnd
 */

bool MeiExporter::writeBeamAndTuplet(const ChordRest *chordRest,
                                     bool &closingBeam, bool &closingTuplet,
                                     bool &closingBeamInTuplet) {
  IF_ASSERT_FAILED(chordRest) { return false; }

  const Beam *beam = (chordRest->beam()) ? toBeam(chordRest->beam()) : nullptr;
  const Tuplet *tuplet =
      (chordRest->tuplet()) ? toTuplet(chordRest->tuplet()) : nullptr;
  const Beam *beamInTuplet = nullptr;

  if (beam && tuplet) {
    if (isFirstExportedBeamElement(beam, chordRest) &&
        isFirstExportedTupletElement(tuplet, chordRest)) {
      if (exportedBeamElementCount(beam) <
          exportedTupletElementCount(tuplet)) {
        beamInTuplet = beam;
        beam = nullptr;
      }
    }
  }
  if (beam) {
    this->writeBeam(beam, chordRest, closingBeam);
  }
  if (tuplet) {
    this->writeTuplet(tuplet, chordRest, closingTuplet);
  }
  if (beamInTuplet) {
    this->writeBeam(beamInTuplet, chordRest, closingBeamInTuplet);
  }

  // Case when the beam was open within the tuplet but not at the beginning
  // (fewer elements) We need to close it first since it is nested in the tuplet
  if (closingTuplet && closingBeam &&
      exportedBeamElementCount(beam) < exportedTupletElementCount(tuplet)) {
    closingBeam = false;
    closingBeamInTuplet = true;
  }

  return true;
}

/**
 * Close beam and tuplet elements according to the parameters calculated in
 * MeiExporter::writeBeamAndTuplet
 */

bool MeiExporter::writeBeamAndTupletEnd(bool closingBeam, bool closingTuplet,
                                        bool closingBeamInTuplet) {
  // non critical asserts
  if (closingBeamInTuplet) {
    assert(isCurrentNode(libmei::Beam()));
    m_currentNode = m_currentNode.parent();
  }

  if (closingTuplet) {
    // Check if the current node is actually a tuplet before asserting
    if (!isCurrentNode(libmei::Tuplet())) {
      LOGD() << "MeiExporter::writeBeamAndTupletEnd ERROR: current node is not "
                "a tuplet, name:"
             << m_currentNode.name();
      // Try to find the tuplet parent node
      pugi::xml_node parent = m_currentNode.parent();
      while (parent && !isNode(parent, String("tuplet"))) {
        parent = parent.parent();
      }
      if (parent) {
        m_currentNode = parent;
      } else {
        LOGD() << "MeiExporter::writeBeamAndTupletEnd ERROR: could not find "
                  "tuplet parent node";
        return false;
      }
    }
    m_currentNode = m_currentNode.parent();
  }

  if (closingBeam) {
    if (!isCurrentNode(libmei::Beam())) {
      LOGD() << "MeiExporter::writeBeamAndTupletEnd no open beam to close";
      return true;
    }
    m_currentNode = m_currentNode.parent();
  }

  return true;
}

/**
 * Write a beam if the ChordRest is the first element of the beam.
 * If the ChordRest is the last, sets the closing flag to true.
 * Also set the MEI `@breaksec` on the previous element when the BeamMode is
 * BEGIN16 or BEGIN32
 */

bool MeiExporter::writeBeam(const Beam *beam, const ChordRest *chordRest,
                            bool &closing) {
  IF_ASSERT_FAILED(beam && chordRest) { return false; }

  const std::vector<ChordRest *> &elements = beam->elements();
  const auto firstExported =
      std::find_if(elements.cbegin(), elements.cend(), shouldExportChordRest);
  if (firstExported == elements.cend()) {
    return true;
  }
  const auto lastExported =
      std::find_if(elements.crbegin(), elements.crend(), shouldExportChordRest);

  // Cross-measure beams are not supported in the export to MEI Basic
  if (beam->elements().front()->measure() !=
      beam->elements().back()->measure()) {
    return true;
  }

  if (*firstExported == chordRest) {
    libmei::Beam meiBeam;
    m_currentNode = m_currentNode.append_child();
    meiBeam.Write(m_currentNode, this->getLayerXmlIdFor(BEAM_L));
  } else if ((chordRest->beamMode() == BeamMode::BEGIN16) ||
             (chordRest->beamMode() == BeamMode::BEGIN32)) {
    pugi::xml_node lastInBeam = this->getLastChordRest(m_currentNode);
    // We have already written one chord/rest element in the beam, the last one
    // is the one we need to modify
    if (lastInBeam) {
      // Create the attribute class to modify the already created XML element
      libmei::InstBeamSecondary beamSecondary;
      beamSecondary.SetBreaksec(Convert::breaksecToMEI(chordRest->beamMode()));
      beamSecondary.WriteBeamSecondary(lastInBeam);
    }
  }

  if (*lastExported == chordRest) {
    closing = true;
  }

  return true;
}

/**
 * Write a bTrem.
 */

bool MeiExporter::writeBTrem(const TremoloSingleChord *tremolo) {
  IF_ASSERT_FAILED(tremolo) { return false; }

  m_currentNode = m_currentNode.append_child();
  libmei::BTrem meiBTrem;
  std::string xmlId = this->getXmlIdFor(tremolo, 'b');
  meiBTrem.Write(m_currentNode, xmlId);

  return true;
}

/**
 * Write a clef.
 */

bool MeiExporter::writeClef(const Clef *clef, const Measure *measure,
                            const Segment *seg, int staffN) {
  IF_ASSERT_FAILED(clef) { return false; }

  if (clef->isHeader()) {
    return true;
  }

  pugi::xml_node clefNode = m_currentNode.append_child();
  libmei::Clef meiClef = Convert::clefToMEI(clef->clefType());
  Convert::colorToMEI(clef, meiClef);
  std::string xmlId = this->getXmlIdFor(clef, 'c');
  meiClef.Write(clefNode, xmlId);

  // Attach timing so parsers can place/activate the clef correctly.
  if (measure && seg) {
    Fraction rel = seg->tick() - measure->tick(); // Ticks within the measure
    double t = Convert::tstampFromFraction(rel, measure->timesig());

    // Prefer the formal MEI @tstamp if libmei exposes it; otherwise write raw
    // attributes. (This keeps us robust across libmei versions.)
    clefNode.append_attribute("beat") =
        formatDecimalStr(t, 4); // Beats in measure, e.g. "2.7500"
    clefNode.append_attribute("staff") =
        staffN; // Explicit, disambiguates cross-staff contexts
  }

  // Pianomania: export clef anchor position in inches.
  PointF clefPos = clef->pagePos();
  double cx = clefPos.x() / DPI;
  double cy = toBottomLeftInches(clefPos.y());
  clefNode.append_attribute("pm:x") = formatDecimalStr(cx, 3).c_str();
  clefNode.append_attribute("pm:y") = formatDecimalStr(cy, 3).c_str();

  return true;
}

/**
 * Write a chord (chord or note).
 * If the Chord is a chord, write its notes with MeiExporter::writeNote.
 * Also write the beam and tuplet (opening and closing) and fill the control
 * event list pointing to it.
 */

bool MeiExporter::writeChord(const Chord *chord, const Staff *staff) {
  IF_ASSERT_FAILED(chord && staff) { return false; }

  std::vector<const Note *> notesToExport = visibleNotes(chord);
  if (notesToExport.empty()) {
    return true;
  }

  if (chord->graceNotes().size() > 0) {
    this->writeGraceGrp(chord, staff);
  }

  bool closingBeam = false;
  bool closingTuplet = false;
  bool closingBeamInTuplet = false;
  this->writeBeamAndTuplet(chord, closingBeam, closingTuplet,
                           closingBeamInTuplet);

  bool isBTrem = (chord->tremoloChordType() == TremoloChordType::TremoloSingle);
  if (isBTrem) {
    this->writeBTrem(chord->tremoloSingleChord());
  }

  bool isChord = (notesToExport.size() > 1);
  if (isChord) {
    // We need to create a <chord> before writing the notes
    m_currentNode = m_currentNode.append_child();
    libmei::Chord meiChord;
    meiChord.SetDur(Convert::durToMEI(chord->durationType().type()));
    if (chord->dots()) {
      meiChord.SetDots(chord->dots());
    }
    this->writeBeamTypeAtt(chord, meiChord);
    this->writeStaffIdentAtt(chord, staff, meiChord);
    this->writeStemAtt(chord, meiChord);
    this->writeArtics(chord);
    this->writeVerses(chord);
    std::string xmlId = this->getXmlIdFor(chord, 'c');

    // Add stem length attribute if the note has a stem.
    if (chord && chord->durationType().type() != DurationType::V_WHOLE) {
      double stemLength = getStemLength(chord);
      if (stemLength > 0.0) {
        meiChord.SetStemLen(stemLength);
      }
    }

    meiChord.Write(m_currentNode, xmlId);

    // Add beat attribute.
    double beatPosition = calculateBeatPosition(chord);
    m_currentNode.append_attribute("beat") = formatBeatPosition(beatPosition);

    // Pianomania: export chord anchor position in inches.
    PointF chordPos = chord->pagePos();
    double cx = chordPos.x() / DPI;
    double cy = toBottomLeftInches(chordPos.y());
    std::string chordXY = formatDecimalStr(cx, 3) + std::string(",") +
                         formatDecimalStr(cy, 3);
    m_currentNode.append_attribute("pm:xy") = chordXY.c_str();

    this->fillControlEventMap(xmlId, chord);
  }

  for (const Note *note : notesToExport) {
    this->writeNote(note, chord, staff, isChord);
  }

  if (isChord) {
    // This is the end of the <chord> - non critical assert
    assert(isCurrentNode(libmei::Chord()));
    m_currentNode = m_currentNode.parent();
  }

  if (isBTrem) {
    // This is the end of the <bTrem> - non critical assert
    assert(isCurrentNode(libmei::BTrem()));
    m_currentNode = m_currentNode.parent();
  }

  this->writeBeamAndTupletEnd(closingBeam, closingTuplet, closingBeamInTuplet);

  if (chord->graceNotes().size() > 0) {
    this->writeGraceGrp(chord, staff, true);
  }

  return true;
}

bool MeiExporter::writeGraceChord(const Chord *graceChord,
                                  const Chord *parentChord, const Staff *staff,
                                  bool isAfter) {
  IF_ASSERT_FAILED(graceChord && parentChord && staff) { return false; }

  std::vector<const Note *> notesToExport = visibleNotes(graceChord);
  if (notesToExport.empty()) {
    return true;
  }

  bool closingBeam = false;
  bool closingTuplet = false;
  bool closingBeamInTuplet = false;
  this->writeBeamAndTuplet(graceChord, closingBeam, closingTuplet,
                           closingBeamInTuplet);

  bool isBTrem =
      (graceChord->tremoloChordType() == TremoloChordType::TremoloSingle);
  if (isBTrem) {
    this->writeBTrem(graceChord->tremoloSingleChord());
  }

  bool isChord = (notesToExport.size() > 1);
  if (isChord) {
    // We need to create a <chord> before writing the notes
    m_currentNode = m_currentNode.append_child();
    libmei::Chord meiChord;
    meiChord.SetDur(Convert::durToMEI(graceChord->durationType().type()));
    if (graceChord->dots()) {
      meiChord.SetDots(graceChord->dots());
    }
    this->writeBeamTypeAtt(graceChord, meiChord);
    this->writeStaffIdentAtt(graceChord, staff, meiChord);
    this->writeStemAtt(graceChord, meiChord);
    this->writeArtics(graceChord);
    this->writeVerses(graceChord);
    std::string xmlId = this->getXmlIdFor(graceChord, 'c');

    // Add stem length attribute if the note has a stem.
    if (graceChord &&
        graceChord->durationType().type() != DurationType::V_WHOLE) {
      double stemLength = getStemLength(graceChord);
      if (stemLength > 0.0) {
        meiChord.SetStemLen(stemLength);
      }
    }

    meiChord.Write(m_currentNode, xmlId);

    // Add beat attribute for grace chord using special calculation.
    double beatPosition =
        calculateGraceNoteBeatPosition(graceChord, parentChord, isAfter);
    m_currentNode.append_attribute("beat") = formatBeatPosition(beatPosition);

    // Pianomania: export grace chord anchor position in inches.
    PointF graceChordPos = graceChord->pagePos();
    double gcx = graceChordPos.x() / DPI;
    double gcy = toBottomLeftInches(graceChordPos.y());
    std::string graceChordXY = formatDecimalStr(gcx, 3) + std::string(",") +
                              formatDecimalStr(gcy, 3);
    m_currentNode.append_attribute("pm:xy") = graceChordXY.c_str();

    this->fillControlEventMap(xmlId, graceChord, false);
  }

  for (const Note *note : notesToExport) {
    this->writeGraceNote(note, graceChord, parentChord, staff, isChord,
                         isAfter);
  }

  if (isChord) {
    // This is the end of the <chord> - non critical assert
    assert(isCurrentNode(libmei::Chord()));
    m_currentNode = m_currentNode.parent();
  }

  if (isBTrem) {
    // This is the end of the <bTrem> - non critical assert
    assert(isCurrentNode(libmei::BTrem()));
    m_currentNode = m_currentNode.parent();
  }

  this->writeBeamAndTupletEnd(closingBeam, closingTuplet, closingBeamInTuplet);

  return true;
}

/**
 * Write a graceGrp placed before or after the chord / note.
 * Loop through the notes of the group and write them.
 */

bool MeiExporter::writeGraceGrp(const Chord *chord, const Staff *staff,
                                bool isAfter) {
  IF_ASSERT_FAILED(chord) { return false; }

  GraceNotesGroup &graceNotes =
      (isAfter) ? chord->graceNotesAfter() : chord->graceNotesBefore();

  if (graceNotes.empty()) {
    return true;
  }

  const bool hasVisibleGraceNotes =
      std::any_of(graceNotes.cbegin(), graceNotes.cend(),
                  [](const Chord *graceChord) {
                    return !visibleNotes(graceChord).empty();
                  });
  if (!hasVisibleGraceNotes) {
    return true;
  }

  m_currentNode = m_currentNode.append_child();

  libmei::GraceGrp meiGraceGrp;
  auto [meiAttach, meiGrace] =
      Convert::gracegrpToMEI(isAfter, graceNotes.front()->noteType());
  meiGraceGrp.SetAttach(meiAttach);
  meiGraceGrp.SetGrace(meiGrace);
  meiGraceGrp.Write(m_currentNode, this->getLayerXmlIdFor(GRACEGRP_L));

  for (auto graceChord : graceNotes) {
    this->writeGraceChord(graceChord, chord, staff, isAfter);
  }

  // non critical assert
  assert(isCurrentNode(meiGraceGrp));
  m_currentNode = m_currentNode.parent();

  return true;
}

/**
 * Write a note (single note or chord note).
 * For single notes, also writes the duration and the beam and stem attributes.
 */

bool MeiExporter::writeNote(const Note *note, const Chord *chord,
                            const Staff *staff, bool isChord) {
  if (!shouldExportNote(note)) {
    return true;
  }

  IF_ASSERT_FAILED(note && chord && staff) { return false; }

  Interval interval = staff->part()->instrument()->transpose();
  auto [meiNote, meiAccid] =
      Convert::pitchToMEI(note, note->accidental(), interval);
  m_currentNode = m_currentNode.append_child();
  if (!isChord) {
    meiNote.SetDur(Convert::durToMEI(chord->durationType().type()));
    if (chord->dots()) {
      meiNote.SetDots(chord->dots());
    }
    this->writeBeamTypeAtt(chord, meiNote);
    this->writeStaffIdentAtt(chord, staff, meiNote);
    this->writeStemAtt(chord, meiNote);
    this->writeArtics(chord);
    this->writeVerses(chord);

    // Add stem length attribute if the note has a stem
    if (chord && chord->durationType().type() != DurationType::V_WHOLE) {
      double stemLength = getStemLength(chord);
      if (stemLength > 0.0) {
        meiNote.SetStemLen(stemLength);
      }
    }
  } else {
    // Add stem direction for notes within chords.
    this->writeStemAtt(chord, meiNote);
  }

  Convert::colorToMEI(note, meiNote);
  std::string xmlId = this->getXmlIdFor(note, 'n');
  meiNote.Write(m_currentNode, xmlId);
  m_noteXmlIdCache[note] = xmlId;

  // Add beat attribute.
  double beatPosition = calculateBeatPosition(chord);
  const std::string beat = formatBeatPosition(beatPosition);
  m_currentNode.append_attribute("beat") = beat.c_str();

  // Pianomania: export notehead anchor position in inches.
  PointF notePos = note->pagePos();
  double nx = notePos.x() / DPI;
  double ny = toBottomLeftInches(notePos.y());
  std::string noteXY = formatDecimalStr(nx, 3) + std::string(",") +
                       formatDecimalStr(ny, 3);
  m_currentNode.append_attribute("pm:xy") = noteXY.c_str();

  // Use precomputed index assigned in MeiExporter::assignIndicesForMeasure.
  auto it =
      m_noteIdxAssignment.find(note); // Find the note in the precomputed index.
  int idx = 0;
  if (it != m_noteIdxAssignment.end()) {
    idx = it->second;
    m_currentNode.append_attribute("idx") =
        idx; // Append the index to the current node.
  } else {
    if (m_requirePrecomputedPianomaniaIndices) {
      LOGE() << "Coordinated Pianomania MEI export is missing a precomputed note index";
      return false;
    }
    // Fallback: generate a new index for this note if it wasn't precomputed
    // This can happen with certain edge cases in grace notes or other complex
    // scenarios
    idx = updateNoteIndex(note->ppitch());
    m_currentNode.append_attribute("idx") = idx;
  }

  m_pianomaniaNoteRecords.push_back(
      {note, xmlId, this->getXmlIdFor(chord, 'c'),
       this->getMeasureXmlId(chord->measure()), beat, idx});

  // Add held attribute if the note has the PIANOMANIA_HELD_NOTE property set to
  // true
  if (note->getProperty(mu::engraving::Pid::PIANOMANIA_HELD_NOTE).toBool()) {
    m_currentNode.append_attribute("held") = "true";
  }
  appendPianomaniaHeldPulseAttributes(m_currentNode, note);
  if (!appendPianomaniaHeldPitchCurveAttribute(m_currentNode, note)) {
    return false;
  }
  // Add shake attribute if the note has the PIANOMANIA_SHAKE_NOTE property set
  // to true
  if (note->getProperty(mu::engraving::Pid::PIANOMANIA_SHAKE_NOTE).toBool()) {
    m_currentNode.append_attribute("shake") = "true";
  }
  int handValue = note->getProperty(mu::engraving::Pid::PIANOMANIA_HAND).toInt();
  if (handValue == static_cast<int>(mu::engraving::Note::PianomaniaHand::Left)) {
    m_currentNode.append_attribute("hand") = "left";
  } else if (handValue ==
             static_cast<int>(mu::engraving::Note::PianomaniaHand::Right)) {
    m_currentNode.append_attribute("hand") = "right";
  }

  if (!isChord) {
    this->fillControlEventMap(xmlId, chord);
  }

  if (note->tieFor()) {
    m_startingControlEventList.push_back(
        std::make_pair(note->tieFor(), "#" + xmlId));
  }
  if (note->tieBack()) {
    m_endingControlEventMap[note->tieBack()] = "#" + xmlId;
  }

  for (const Spanner *spanner : note->spannerFor()) {
    if (spanner && spanner->isGlissando()) {
      m_startingControlEventList.push_back(
          std::make_pair(spanner, "#" + xmlId));
    }
  }
  for (const Spanner *spanner : note->spannerBack()) {
    if (spanner && spanner->isGlissando()) {
      m_endingControlEventMap[spanner] = "#" + xmlId;
    }
  }

  for (const EngravingItem *element : note->el()) {
    if (element->isFingering()) {
      m_startingControlEventList.push_back(
          std::make_pair(element, "#" + xmlId));
    }
  }

  if (meiAccid.HasAccid() || meiAccid.HasAccidGes()) {
    pugi::xml_node accidNode = m_currentNode.append_child();
    Accidental *acc = note->accidental();
    PointF accPos;
    bool hasAccPos = false;
    if (acc) {
      Convert::colorToMEI(acc, meiAccid);
      std::string xmlIdAcc = this->getXmlIdFor(acc, 'a');
      meiAccid.Write(accidNode, xmlIdAcc);
      accPos = acc->pagePos();
      hasAccPos = true;
    } else {
      meiAccid.Write(accidNode, this->getLayerXmlIdFor(ACCID_L));
      PointF notePos = note->pagePos();
      double accidentalDistanceMm = note->style().styleMM(Sid::accidentalDistance);
      static constexpr double MM_PER_INCH = 25.4;
      double fallbackOffset = (accidentalDistanceMm / MM_PER_INCH) * DPI;
      notePos.setX(notePos.x() - fallbackOffset);
      accPos = notePos;
      hasAccPos = true;
    }

    if (hasAccPos && meiAccid.HasAccid()) {
      double ax = accPos.x() / DPI;
      double ay = toBottomLeftInches(accPos.y());
      std::string accXY = formatDecimalStr(ax, 3) + std::string(",") +
                          formatDecimalStr(ay, 3);
      accidNode.append_attribute("pm:xy") = accXY.c_str();
    }
  }

  // non critical assert
  assert(isCurrentNode(meiNote));
  m_currentNode = m_currentNode.parent();

  return true;
}

/**
 * Write a rest (rest, mRest, and space).
 */

bool MeiExporter::writeRest(const Rest *rest, const Staff *staff) {
  IF_ASSERT_FAILED(rest) { return false; }

  // If the rest is a "gap" (invisible rest) or not visible, skip it.
  if (rest->isGap() || !rest->visible()) {
    this->collectPendingControlEvents(rest);
    return true;
  }

  // measure rest
  if (rest->durationType() == DurationType::V_MEASURE) {
    pugi::xml_node mRestNode = m_currentNode.append_child();
    libmei::MRest meiMRest;
    Convert::colorToMEI(rest, meiMRest);
    std::string xmlId = this->getXmlIdFor(rest, 'm');
    meiMRest.Write(mRestNode, xmlId);
    this->fillControlEventMap(xmlId, rest);

    // Add beat attribute
    double beatPosition = calculateBeatPosition(rest);
    mRestNode.append_attribute("beat") = formatBeatPosition(beatPosition);

    // Add yOffset attribute
    double yOffset = getRestYOffset(rest);
    mRestNode.append_attribute("yOffset") = formatDecimalStr(yOffset);

    int handValue = rest->getProperty(mu::engraving::Pid::PIANOMANIA_HAND).toInt();
    if (handValue == static_cast<int>(mu::engraving::Note::PianomaniaHand::Left)) {
      mRestNode.append_attribute("hand") = "left";
    } else if (handValue ==
               static_cast<int>(mu::engraving::Note::PianomaniaHand::Right)) {
      mRestNode.append_attribute("hand") = "right";
    }
  } else {
    bool closingBeam = false;
    bool closingTuplet = false;
    bool closingBeamInTuplet = false;
    this->writeBeamAndTuplet(rest, closingBeam, closingTuplet,
                             closingBeamInTuplet);

    pugi::xml_node restNode = m_currentNode.append_child();
    libmei::Rest meiRest;
    meiRest.SetDur(Convert::durToMEI(rest->durationType().type()));
    if (rest->dots()) {
      meiRest.SetDots(rest->dots());
    }
    if (rest->visible()) {
      Convert::colorToMEI(rest, meiRest);
    }
    this->writeBeamTypeAtt(rest, meiRest);
    this->writeStaffIdentAtt(rest, staff, meiRest);
    // this->writeVerses(rest);
    const char prefix = (rest->visible()) ? 'r' : 's';
    std::string xmlId = this->getXmlIdFor(rest, prefix);
    meiRest.Write(restNode, xmlId);
    this->fillControlEventMap(xmlId, rest);

    // Add beat attribute
    double beatPosition = calculateBeatPosition(rest);
    restNode.append_attribute("beat") = formatBeatPosition(beatPosition);

    // Add yOffset attribute
    double yOffset = getRestYOffset(rest);
    restNode.append_attribute("yOffset") = formatDecimalStr(yOffset);

    int handValue = rest->getProperty(mu::engraving::Pid::PIANOMANIA_HAND).toInt();
    if (handValue == static_cast<int>(mu::engraving::Note::PianomaniaHand::Left)) {
      restNode.append_attribute("hand") = "left";
    } else if (handValue ==
               static_cast<int>(mu::engraving::Note::PianomaniaHand::Right)) {
      restNode.append_attribute("hand") = "right";
    }

    // Pianomania: export rest position in inches.
    PointF restPos = rest->pagePos();
    double restX = restPos.x() / DPI;
    double restY = toBottomLeftInches(restPos.y());
    std::string restXY = formatDecimalStr(restX, 3) + std::string(",") +
                         formatDecimalStr(restY, 3);
    restNode.append_attribute("pm:xy") = restXY.c_str();

    // Change invisible rests to space by simply adjusting the element name
    if (!rest->visible() || rest->isGap()) {
      restNode.set_name("space");
    }

    this->writeBeamAndTupletEnd(closingBeam, closingTuplet,
                                closingBeamInTuplet);
  }

  return true;
}

/**
 * Write a measure repeat
 */

bool MeiExporter::writeMRpt(const MeasureRepeat *measureRepeat) {
  IF_ASSERT_FAILED(measureRepeat) { return false; }

  libmei::MRpt meiMRpt;
  Convert::colorToMEI(measureRepeat, meiMRpt);
  meiMRpt.SetExpand(libmei::BOOLEAN_false);
  pugi::xml_node mRptNode = m_currentNode.append_child();
  meiMRpt.Write(mRptNode, this->getXmlIdFor(measureRepeat, 'm'));

  m_currentNode = m_currentNode.parent();

  return true;
}

/**
 * Write a syl with the corresponding text syllable and the elision type.
 * The elision type is passed to the Convert methods that deals with the
 * adjustment of @con and @wordpos.
 */

bool MeiExporter::writeSyl(const Lyrics *lyrics, const String &text,
                           ElisionType elision) {
  libmei::Syl meiSyl = Convert::sylToMEI(lyrics, elision);
  pugi::xml_node sylNode = m_currentNode.append_child();
  meiSyl.Write(sylNode, this->getLayerXmlIdFor(SYL_L));
  sylNode.text().set(text.toStdString().c_str());

  return true;
}

/**
 * Write a tuplet if the ChordRest is the first element of the tuplet.
 * If the ChordRest is the last, sets the closing flag to true.
 */

bool MeiExporter::writeTuplet(const Tuplet *tuplet, const EngravingItem *item,
                              bool &closing) {
  IF_ASSERT_FAILED(tuplet && item) { return false; }

  const std::vector<DurationElement *> &elements = tuplet->elements();
  const auto firstExported = std::find_if(
      elements.cbegin(), elements.cend(), shouldExportDurationElement);
  if (firstExported == elements.cend()) {
    return true;
  }
  const auto lastExported = std::find_if(
      elements.crbegin(), elements.crend(), shouldExportDurationElement);

  if (*firstExported == item) {
    // recursive call for handling nested tuplets
    // nearly works except for closing which is happening to early (after the
    // first note) when a nested tuplet is ending at the same time as its parent
    /**
    if (tuplet->tuplet()) {
        writeTuplet(toTuplet(tuplet->tuplet()), tuplet, closing);
    }
    if (item->isTuplet()) {
        LOGD() << "MeiExporter::writeTuplet nested tuplet export not fully
    supported";
    }
    */
    libmei::Tuplet meiTuplet = Convert::tupletToMEI(tuplet);
    m_currentNode = m_currentNode.append_child();
    std::string xmlId = this->getXmlIdFor(tuplet, 't');
    meiTuplet.Write(m_currentNode, xmlId);
    if (!appendPmTupletGeometry(m_currentNode, tuplet)) {
      LOGE() << "MEI export requires complete resolved tuplet geometry";
      return false;
    }
  }

  if (*lastExported == item) {
    closing = true;
  }

  return true;
}

/**
 * Write the verses attached to a ChordRest
 */

bool MeiExporter::writeVerses(const ChordRest *chordRest) {
  IF_ASSERT_FAILED(chordRest) { return false; }

  for (const Lyrics *lyrics : chordRest->lyrics()) {
    this->writeVerse(lyrics);
  }

  return true;
}

/**
 * Write a verse for a Lyrics and the syl - one or more with elisions.
 * If the Lyrics text has elision(s), then splits them into distinct MEI syl.
 */

bool MeiExporter::writeVerse(const Lyrics *lyrics) {
  IF_ASSERT_FAILED(lyrics) { return false; }

  libmei::Verse meiVerse;
  meiVerse.SetN(String::number(lyrics->verse() + 1).toStdString());
  if (lyrics->propertyFlags(engraving::Pid::PLACEMENT) ==
      engraving::PropertyFlags::UNSTYLED) {
    meiVerse.SetPlace(Convert::placeToMEI(lyrics->placement()));
  }
  Convert::colorToMEI(lyrics, meiVerse);
  m_currentNode = m_currentNode.append_child();
  std::string xmlId = this->getXmlIdFor(lyrics, 'v');
  meiVerse.Write(m_currentNode, xmlId);

  // Split the syllable into line blocks
  Convert::textWithSmufl lineBlocks;
  Convert::textToMEI(lineBlocks, String(lyrics->plainText()));

  // If we have more than one line block we assume to have elision
  // Ideally we should check that SMuFL line block do contain only an elision
  // character It also means that any SMuFL special character in the lyrics will
  // be considered to be an elision connector
  ElisionType elision = (lineBlocks.size() > 1) ? ElisionFirst : ElisionNone;

  for (auto &lineBlock : lineBlocks) {
    // For now assume any SMuFL line block to be an elision connector.
    // That means we simply skip them.
    if (lineBlock.first) {
      continue;
    }
    // If the line block in the last one with an elision, mark it as such
    if ((elision == ElisionMiddle) && (&lineBlock == &lineBlocks.back())) {
      elision = ElisionLast;
    }
    // Create a /syl for each text line block
    this->writeSyl(lyrics, lineBlock.second, elision);
    // Next one will be a middle (or last) elision line block
    elision = ElisionMiddle;
  }

  // This is the end of the <verse> - non critical assert
  assert(isCurrentNode(libmei::Verse()));
  m_currentNode = m_currentNode.parent();

  return true;
}

//---------------------------------------------------------
// write MEI control events
//---------------------------------------------------------

/**
 * Write a arpeg.
 */

bool MeiExporter::writeArpeg(const Arpeggio *arpeggio,
                             const std::string &startid) {
  IF_ASSERT_FAILED(arpeggio) { return false; }

  pugi::xml_node arpegNode = m_currentNode.append_child();
  libmei::Arpeg meiArpeg = Convert::arpegToMEI(arpeggio);
  meiArpeg.SetStartid(startid);

  meiArpeg.Write(arpegNode, this->getXmlIdFor(arpeggio, 'a'));

  // If the arpeggio is spanning to a lower staff, keep it as open control event
  if (arpeggio->span() > 1) {
    m_openControlEventMap[arpeggio] = arpegNode;
  }

  return true;
}

/**
 * Write a breath (i.e., breath or caesura).
 */

bool MeiExporter::writeBreath(const Breath *breath,
                              const std::string &startid) {
  IF_ASSERT_FAILED(breath) { return false; }

  pugi::xml_node breathNode = m_currentNode.append_child();
  if (breath->isCaesura()) {
    libmei::Caesura meiCaesura = Convert::caesuraToMEI(breath);
    meiCaesura.SetStartid(startid);
    meiCaesura.Write(breathNode, this->getXmlIdFor(breath, 'c'));
  } else {
    libmei::Breath meiBreath = Convert::breathToMEI(breath);
    meiBreath.SetStartid(startid);
    meiBreath.Write(breathNode, this->getXmlIdFor(breath, 'b'));
  }

  return true;
}

/**
 * Write a dir and its text content.
 */

bool MeiExporter::writeDir(const TextBase *dir, const std::string &startid) {
  IF_ASSERT_FAILED(dir) { return false; }

  StringList meiLines;

  pugi::xml_node dirNode = m_currentNode.append_child();
  libmei::Dir meiDir = Convert::dirToMEI(dir, meiLines);
  meiDir.SetStartid(startid);
  meiDir.Write(dirNode, this->getXmlIdFor(dir, 'd'));

  double yOffset = getDirectiveYOffset(dir);
  dirNode.append_attribute("yOffset") = formatDecimalStr(yOffset, 1);

  if (!appendResolvedCenterBetweenStaves(dirNode, dir)) {
    return false;
  }

  appendCenteredPmPosition(dirNode, dir);

  this->writeLines(dirNode, meiLines);

  return true;
}

/**
 * Write a dir (with extender) and its text content.
 */

bool MeiExporter::writeDir(const TextLineBase *dir,
                           const std::string &startid) {
  IF_ASSERT_FAILED(dir) { return false; }

  const Hairpin *hairpin = dir->isHairpin() ? toHairpin(dir) : nullptr;
  const HairpinSegment *hairpinSegment = nullptr;
  const System *hairpinSystem = nullptr;
  if (hairpin) {
    const auto layout = resolvedHairpinLayout(hairpin);
    if (!layout.has_value()) {
      return false;
    }
    hairpinSegment = layout->first;
    hairpinSystem = layout->second;
  }

  StringList meiLines;

  pugi::xml_node dirNode = m_currentNode.append_child();
  libmei::Dir meiDir = Convert::dirToMEI(dir, meiLines);
  meiDir.SetStartid(startid);
  meiDir.Write(dirNode, this->getXmlIdFor(dir, 'd'));

  double yOffset =
      hairpin ? getHairpinYOffset(hairpin) : getDirectiveYOffset(dir);
  dirNode.append_attribute("yOffset") = formatDecimalStr(yOffset, 1);

  if (hairpin) {
    const bool centered = engraving::rendering::score::SystemLayout::
        elementShouldBeCenteredBetweenStaves(hairpinSegment, hairpinSystem);
    dirNode.append_attribute("centerBetweenStaves") =
        centered ? "true" : "false";
  }

  // For TextLineBase spanners (like hairpins with text), use the first segment
  // position for pm:xy since pageBoundingRect() doesn't work correctly for spanners.
  auto endpoints = getLineEndpointsInches(dir);
  if (endpoints.has_value()) {
    const auto &coords = endpoints.value();
    // Use the start position (x1, y1) as the text position.
    const std::string xStr = formatDecimalStr(coords[0], 3);
    const std::string yStr = formatDecimalStr(coords[1], 3);
    const std::string xyStr = xStr + std::string(",") + yStr;
    dirNode.append_attribute("pm:xy") = xyStr.c_str();
  }
  appendPmLineEndpoints(dirNode, dir, "pm:x1y1x2y2");

  // Export individual segments for multi-system directives.
  if (!dir->segmentsEmpty()) {
    auto makePoint = [this](const PointF &pagePoint) {
      double x = pagePoint.x() / DPI;
      double y = toBottomLeftInches(pagePoint.y());
      return std::pair<double, double>(x, y);
    };

    std::string segmentData;
    for (const SpannerSegment *seg : dir->spannerSegments()) {
      if (!seg) {
        continue;
      }

      PointF segStart = seg->pagePos();
      PointF segEnd = seg->pagePos() + seg->pos2();
      auto [sx, sy] = makePoint(segStart);
      auto [ex, ey] = makePoint(segEnd);

      if (!segmentData.empty()) {
        segmentData += ";";
      }

      segmentData += formatDecimalStr(sx, 3) + std::string(",") +
                     formatDecimalStr(sy, 3) + std::string(",") +
                     formatDecimalStr(ex, 3) + std::string(",") +
                     formatDecimalStr(ey, 3);
    }

    if (!segmentData.empty()) {
      dirNode.append_attribute("pm:segments") = segmentData.c_str();
    }
  }

  this->writeLines(dirNode, meiLines);

  // Add the node to the map of open control events
  this->addNodeToOpenControlEvents(dirNode, dir, startid);

  return true;
}

/**
 * Write a dynam and its text content.
 */

bool MeiExporter::writeDynam(const Dynamic *dynamic,
                             const std::string &startid) {
  IF_ASSERT_FAILED(dynamic) { return false; }

  StringList meiLines;

  pugi::xml_node dynamNode = m_currentNode.append_child();
  libmei::Dynam meiDynam = Convert::dynamToMEI(dynamic, meiLines);
  meiDynam.SetStartid(startid);

  // First write all MEI attributes (including xml:id).
  meiDynam.Write(dynamNode, this->getXmlIdFor(dynamic, 'd'));

  // Get the parent segment.
  const Segment *segment = dynamic->segment();
  if (segment) {
    double yPos = getDynamicYOffset(dynamic);

    // Add yOffset as a custom attribute using formatDecimalStr
    dynamNode.append_attribute("yOffset") = formatDecimalStr(yPos, 1);
  }

  if (!appendResolvedCenterBetweenStaves(dynamNode, dynamic)) {
    return false;
  }

  const auto center = getCenteredInchesFor(dynamic);
  if (center.has_value()) {
    std::string dynamXY = formatDecimalStr(center->first, 3) +
                          std::string(",") +
                          formatDecimalStr(center->second, 3);
    dynamNode.append_attribute("pm:x") = formatDecimalStr(center->first, 3).c_str();
    dynamNode.append_attribute("pm:y") = formatDecimalStr(center->second, 3).c_str();
    dynamNode.append_attribute("pm:xy") = dynamXY.c_str();
  }

  this->writeLines(dynamNode, meiLines);

  return true;
}

/**
 * Write a f (FigureBassItem).
 */

bool MeiExporter::writeF(const FiguredBassItem *figuredBassItem) {
  IF_ASSERT_FAILED(figuredBassItem) { return false; }

  StringList meiLines;

  pugi::xml_node fNode = m_currentNode.append_child();
  libmei::F meiF = Convert::fToMEI(figuredBassItem, meiLines);
  meiF.Write(fNode, this->getXmlIdFor(figuredBassItem, 'f'));

  this->writeLines(fNode, meiLines);

  return true;
}

/**
 * Write a fb (FigureBass).
 */

bool MeiExporter::writeFb(const FiguredBass *figuredBass,
                          const std::string &startid) {
  IF_ASSERT_FAILED(figuredBass) { return false; }

  m_currentNode = m_currentNode.append_child();

  auto [meiHarm, meiFb] = Convert::fbToMEI(figuredBass);
  meiHarm.SetStartid(startid);
  meiHarm.Write(m_currentNode, this->getLayerXmlIdFor(HARM_L));

  m_currentNode = m_currentNode.append_child();
  meiFb.Write(m_currentNode, this->getXmlIdFor(figuredBass, 'f'));

  for (const FiguredBassItem *f : figuredBass->items()) {
    this->writeF(f);
  }

  // This is the end of the <fb> - non critical assert
  assert(isCurrentNode(libmei::Fb()));
  m_currentNode = m_currentNode.parent();

  // This is the end of the <harm> - non critical assert
  assert(isCurrentNode(libmei::Harm()));
  m_currentNode = m_currentNode.parent();

  return true;
}

/**
 * Write a fermata.
 */

bool MeiExporter::writeFermata(const Fermata *fermata,
                               const std::string &startid) {
  IF_ASSERT_FAILED(fermata) { return false; }

  pugi::xml_node fermataNode = m_currentNode.append_child();
  libmei::Fermata meiFermata = Convert::fermataToMEI(fermata);
  meiFermata.SetStartid(startid);

  double yOffset = getFermataYOffset(fermata);
  meiFermata.Write(fermataNode, this->getXmlIdFor(fermata, 'f'));
  fermataNode.append_attribute("yOffset") = formatDecimalStr(yOffset, 1);

  PointF fermataPos = fermata->pagePos();
  double fx = fermataPos.x() / DPI;
  double fy = toBottomLeftInches(fermataPos.y());
  std::string fermataXY = formatDecimalStr(fx, 3) + std::string(",") +
                          formatDecimalStr(fy, 3);
  fermataNode.append_attribute("pm:xy") = fermataXY.c_str();

  return true;
}

/**
 * Write a fermata with a staffNs and tstamp
 */

bool MeiExporter::writeFermata(const Fermata *fermata,
                               const libmei::xsdPositiveInteger_List &staffNs,
                               double tstamp) {
  IF_ASSERT_FAILED(fermata) { return false; }

  pugi::xml_node fermataNode = m_currentNode.append_child();
  libmei::Fermata meiFermata = Convert::fermataToMEI(fermata);
  meiFermata.SetStaff(staffNs);
  meiFermata.SetTstamp(tstamp);

  double yOffset = getFermataYOffset(fermata);
  meiFermata.Write(fermataNode, this->getXmlIdFor(fermata, 'f'));
  fermataNode.append_attribute("yOffset") = formatDecimalStr(yOffset, 1);

  PointF fermataPos = fermata->pagePos();
  double fx = fermataPos.x() / DPI;
  double fy = toBottomLeftInches(fermataPos.y());
  std::string fermataXY = formatDecimalStr(fx, 3) + std::string(",") +
                          formatDecimalStr(fy, 3);
  fermataNode.append_attribute("pm:xy") = fermataXY.c_str();

  return true;
}

/**
 * Write a fing and its text content.
 */

bool MeiExporter::writeFing(const Fingering *fing, const std::string &startid) {
  IF_ASSERT_FAILED(fing) { return false; }

  StringList meiLines;

  pugi::xml_node fingNode = m_currentNode.append_child();
  libmei::Fing meiFing = Convert::fingToMEI(fing, meiLines);
  meiFing.SetStartid(startid);
  meiFing.Write(fingNode, this->getXmlIdFor(fing, 'f'));

  if (const auto fingeringOffsets = getFingeringOffsets(fing, startid);
      fingeringOffsets.has_value()) {
    fingNode.append_attribute("xOffset") =
        formatDecimalStr(fingeringOffsets->first, 1);
    fingNode.append_attribute("yOffset") =
        formatDecimalStr(fingeringOffsets->second, 1);
  }

  appendCenteredPmPosition(fingNode, fing);

  this->writeLines(fingNode, meiLines);

  return true;
}

/**
 * Write a hairpin.
 */
bool MeiExporter::writeHairpin(const Hairpin *hairpin,
                               const std::string &startid) {
  IF_ASSERT_FAILED(hairpin) { return false; }

  if (hairpin->isLineType()) {
    return this->writeDir(dynamic_cast<const TextLineBase *>(hairpin), startid);
  }

  const auto layout = resolvedHairpinLayout(hairpin);
  if (!layout.has_value()) {
    return false;
  }
  const HairpinSegment *firstSegment = layout->first;
  const System *hairpinSystem = layout->second;

  pugi::xml_node hairpinNode = m_currentNode.append_child();
  libmei::Hairpin meiHairpin = Convert::hairpinToMEI(hairpin);
  meiHairpin.SetStartid(startid);

  // First write all MEI attributes (including xml:id).
  meiHairpin.Write(hairpinNode, this->getXmlIdFor(hairpin, 'h'));

  const double yPos = getHairpinYOffset(hairpin);
  hairpinNode.append_attribute("yOffset") = formatDecimalStr(yPos, 1);

  const bool centered = engraving::rendering::score::SystemLayout::
      elementShouldBeCenteredBetweenStaves(firstSegment, hairpinSystem);
  hairpinNode.append_attribute("centerBetweenStaves") =
      centered ? "true" : "false";

  if (!hairpin->segmentsEmpty()) {
    auto makePoint = [this](const PointF &pagePoint) {
      double x = pagePoint.x() / DPI;
      double y = toBottomLeftInches(pagePoint.y());
      return std::pair<double, double>(x, y);
    };

    const HairpinSegment *lastSegment = hairpin->backSegment()
                                             ? toHairpinSegment(hairpin->backSegment())
                                             : nullptr;

    if (firstSegment && lastSegment) {
      auto [x1, y1] = makePoint(firstSegment->pagePos());
      PointF lastEndPos = lastSegment->pagePos() + lastSegment->pos2();
      auto [x2, y2] = makePoint(lastEndPos);

      std::string hairpinXY = formatDecimalStr(x1, 3) + std::string(",") +
                              formatDecimalStr(y1, 3) + std::string(",") +
                              formatDecimalStr(x2, 3) + std::string(",") +
                              formatDecimalStr(y2, 3);
      hairpinNode.append_attribute("pm:x1y1x2y2") = hairpinXY.c_str();

      std::string segmentData;
      std::string hairpinLineData;
      auto appendHairpinLine = [&hairpinLineData, this](int pageIndex,
                                                  double x1,
                                                  double y1,
                                                  double x2,
                                                  double y2) {
        if (!hairpinLineData.empty()) {
          hairpinLineData += ";";
        }

        hairpinLineData += std::to_string(pageIndex) + std::string(",") +
                           formatDecimalStr(x1, 3) + std::string(",") +
                           formatDecimalStr(y1, 3) + std::string(",") +
                           formatDecimalStr(x2, 3) + std::string(",") +
                           formatDecimalStr(y2, 3);
      };

      for (const SpannerSegment *seg : hairpin->spannerSegments()) {
        const HairpinSegment *hairpinSeg = seg->isHairpinSegment()
                                              ? toHairpinSegment(seg)
                                              : nullptr;
        if (!hairpinSeg) {
          continue;
        }

        PointF segStart = hairpinSeg->pagePos();
        PointF segEnd = hairpinSeg->pagePos() + hairpinSeg->pos2();
        auto [sx, sy] = makePoint(segStart);
        auto [ex, ey] = makePoint(segEnd);

        if (!segmentData.empty()) {
          segmentData += ";";
        }

        segmentData += formatDecimalStr(sx, 3) + std::string(",") +
                       formatDecimalStr(sy, 3) + std::string(",") +
                       formatDecimalStr(ex, 3) + std::string(",") +
                       formatDecimalStr(ey, 3);

        const System *segmentSystem = hairpinSeg->system();
        const Page *segmentPage = segmentSystem ? segmentSystem->page() : nullptr;
        int pageIndex = segmentPage ? static_cast<int>(segmentPage->pageNumber()) : 0;

        const PointF *linePoints = hairpinSeg->ldata()->points.data();
        if (linePoints && hairpinSeg->ldata()->npoints >= 4) {
          PointF line1Start = hairpinSeg->pagePos() + linePoints[0];
          PointF line1End = hairpinSeg->pagePos() + linePoints[1];
          auto [l1sx, l1sy] = makePoint(line1Start);
          auto [l1ex, l1ey] = makePoint(line1End);
          appendHairpinLine(pageIndex, l1sx, l1sy, l1ex, l1ey);

          PointF line2Start = hairpinSeg->pagePos() + linePoints[2];
          PointF line2End = hairpinSeg->pagePos() + linePoints[3];
          auto [l2sx, l2sy] = makePoint(line2Start);
          auto [l2ex, l2ey] = makePoint(line2End);
          appendHairpinLine(pageIndex, l2sx, l2sy, l2ex, l2ey);
        }
      }

      if (!segmentData.empty()) {
        hairpinNode.append_attribute("pm:segments") = segmentData.c_str();
      }

      if (!hairpinLineData.empty()) {
        hairpinNode.append_attribute("pm:hairpin-lines") =
            hairpinLineData.c_str();
      }
    }
  }

  std::vector<const Note *> coveredNotes = collectHairpinCoveredNotes(hairpin);
  if (!coveredNotes.empty()) {
    auto buildNoteIdList = [this](const std::vector<const Note *> &notes) {
      std::string idList;
      for (const Note *note : notes) {
        if (!shouldExportNote(note)) {
          continue;
        }

        std::string noteId;
        auto cachedId = m_noteXmlIdCache.find(note);
        if (cachedId != m_noteXmlIdCache.end()) {
          noteId = cachedId->second;
        } else {
          noteId = getXmlIdFor(note, 'n');
          if (!noteId.empty()) {
            m_noteXmlIdCache[note] = noteId;
          }
        }

        if (noteId.empty()) {
          continue;
        }

        if (!idList.empty()) {
          idList += ' ';
        }
        idList += noteId;
      }
      return idList;
    };

    std::string coveredIds = buildNoteIdList(coveredNotes);
    if (!coveredIds.empty()) {
      hairpinNode.append_attribute("pm:covered-id") = coveredIds.c_str();
    }
  }

  // Add the node to the map of open control events
  this->addNodeToOpenControlEvents(hairpinNode, hairpin, startid);

  return true;
}

/**
 * Write a harm and its text content.
 */

bool MeiExporter::writeHarm(const Harmony *harmony,
                            const std::string &startid) {
  IF_ASSERT_FAILED(harmony) { return false; }

  StringList meiLines;

  pugi::xml_node harmNode = m_currentNode.append_child();
  libmei::Harm meiHarm = Convert::harmToMEI(harmony, meiLines);
  meiHarm.SetStartid(startid);
  meiHarm.Write(harmNode, this->getXmlIdFor(harmony, 'h'));

  appendCenteredPmPosition(harmNode, harmony);

  this->writeLines(harmNode, meiLines);

  return true;
}

/**
 * Write a harpPedal.
 */

bool MeiExporter::writeHarpPedal(const HarpPedalDiagram *harpPedalDiagram,
                                 const std::string &startid) {
  IF_ASSERT_FAILED(harpPedalDiagram) { return false; }
  if (!harpPedalDiagram->isDiagram()) {
    return true;
  }

  pugi::xml_node harpPedalNode = m_currentNode.append_child();
  libmei::HarpPedal meiHarpPedal = Convert::harpPedalToMEI(harpPedalDiagram);
  meiHarpPedal.SetStartid(startid);
  meiHarpPedal.Write(harpPedalNode, this->getXmlIdFor(harpPedalDiagram, 'h'));

  return true;
}

/**
 * Write a octave (ottava).
 */

bool MeiExporter::writeOctave(const Ottava *ottava,
                              const std::string &startid) {
  IF_ASSERT_FAILED(ottava) { return false; }

  pugi::xml_node octaveNode = m_currentNode.append_child();
  libmei::Octave meiOctave = Convert::octaveToMEI(ottava);
  meiOctave.SetStartid(startid);

  meiOctave.Write(octaveNode, this->getXmlIdFor(ottava, 'o'));

  // Add the yOffset attribute.
  double yOffset = getOctaveYOffset(ottava);
  octaveNode.append_attribute("yOffset") = formatDecimalStr(yOffset, 1);

  // Add the endHookLength attribute.
  double endHookHeight = getOctaveEndHookHeight(ottava);
  octaveNode.append_attribute("hook.len") = formatDecimalStr(endHookHeight, 1);

  if (!ottava->segmentsEmpty()) {
    auto makePoint = [this](const PointF &pagePoint) {
      double x = pagePoint.x() / DPI;
      double y = toBottomLeftInches(pagePoint.y());
      return std::pair<double, double>(x, y);
    };

    const OttavaSegment *firstSegment = ottava->frontSegment()
                                            ? toOttavaSegment(ottava->frontSegment())
                                            : nullptr;
    const OttavaSegment *lastSegment = ottava->backSegment()
                                           ? toOttavaSegment(ottava->backSegment())
                                           : nullptr;

    if (firstSegment && lastSegment) {
      auto [x1, y1] = makePoint(firstSegment->pagePos());
      PointF lastEndPos = lastSegment->pagePos() + lastSegment->pos2();
      auto [x2, y2] = makePoint(lastEndPos);

      std::string octaveXY = formatDecimalStr(x1, 3) + std::string(",") +
                             formatDecimalStr(y1, 3) + std::string(",") +
                             formatDecimalStr(x2, 3) + std::string(",") +
                             formatDecimalStr(y2, 3);
      octaveNode.append_attribute("pm:x1y1x2y2") = octaveXY.c_str();

      std::string segmentData;
      for (const SpannerSegment *seg : ottava->spannerSegments()) {
        const OttavaSegment *ottavaSeg = seg->isOttavaSegment()
                                             ? toOttavaSegment(seg)
                                             : nullptr;
        if (!ottavaSeg) {
          continue;
        }

        PointF segStart = ottavaSeg->pagePos();
        PointF segEnd = ottavaSeg->pagePos() + ottavaSeg->pos2();
        auto [sx, sy] = makePoint(segStart);
        auto [ex, ey] = makePoint(segEnd);

        if (!segmentData.empty()) {
          segmentData += ";";
        }

        segmentData += formatDecimalStr(sx, 3) + std::string(",") +
                       formatDecimalStr(sy, 3) + std::string(",") +
                       formatDecimalStr(ex, 3) + std::string(",") +
                       formatDecimalStr(ey, 3);
      }

      if (!segmentData.empty()) {
        octaveNode.append_attribute("pm:segments") = segmentData.c_str();
      }
    }
  }

  // Add the node to the map of open control events.
  this->addNodeToOpenControlEvents(octaveNode, ottava, startid);

  return true;
}

/**
 * Write a ornament.
 * Select the appropriate corresponding MEI element for it.
 */

bool MeiExporter::writeOrnament(const Ornament *ornament,
                                const std::string &startid) {
  IF_ASSERT_FAILED(ornament) { return false; }

  pugi::xml_node ornamentNode = m_currentNode.append_child();
  if (Convert::isMordent(ornament)) {
    libmei::Mordent meiMordent = Convert::mordentToMEI(ornament);
    meiMordent.SetStartid(startid);
    meiMordent.Write(ornamentNode, this->getXmlIdFor(ornament, 'm'));
  } else if (Convert::isTrill(ornament)) {
    libmei::Trill meiTrill = Convert::trillToMEI(ornament);
    meiTrill.SetStartid(startid);
    meiTrill.Write(ornamentNode, this->getXmlIdFor(ornament, 't'));
  } else if (Convert::isTurn(ornament)) {
    libmei::Turn meiTurn = Convert::turnToMEI(ornament);
    meiTurn.SetStartid(startid);
    meiTurn.Write(ornamentNode, this->getXmlIdFor(ornament, 't'));
  } else {
    libmei::Ornam meiOrnam = Convert::ornamToMEI(ornament);
    meiOrnam.SetStartid(startid);
    meiOrnam.Write(ornamentNode, this->getXmlIdFor(ornament, 'o'));
  }

  double yOffset = getOrnamentYOffset(ornament);
  ornamentNode.append_attribute("yOffset") = formatDecimalStr(yOffset, 1).c_str();

  return true;
}

/**
 * Write a pedal.
 */

bool MeiExporter::writePedal(const Pedal *pedal, const std::string &startid) {
  IF_ASSERT_FAILED(pedal) { return false; }

  pugi::xml_node pedalNode = m_currentNode.append_child();
  libmei::Pedal meiPedal = Convert::pedalToMEI(pedal);
  meiPedal.SetStartid(startid);

  meiPedal.Write(pedalNode, this->getXmlIdFor(pedal, 'p'));

  // Pianomania: export pedal start and end positions in inches when both
  // spanner anchors resolved. Broken source connectors still export as MEI.
  const EngravingItem *startElement = pedal->startElement();
  const EngravingItem *endElement = pedal->endElement();
  if (startElement && endElement) {
    PointF startPos = startElement->pagePos();
    PointF endPos = endElement->pagePos();
    double x1 = startPos.x() / DPI;
    double y1 = toBottomLeftInches(startPos.y());
    double x2 = endPos.x() / DPI;
    double y2 = toBottomLeftInches(endPos.y());
    std::string pedalXY = formatDecimalStr(x1, 3) + std::string(",") +
                         formatDecimalStr(y1, 3) + std::string(",") +
                         formatDecimalStr(x2, 3) + std::string(",") +
                         formatDecimalStr(y2, 3);
    pedalNode.append_attribute("pm:x1y1x2y2") = pedalXY.c_str();
  } else {
    LOGD() << "MeiExporter::writePedal skipping pm:x1y1x2y2 for "
              "unresolved pedal spanner";
  }

  // Add the node to the map of open control events
  this->addNodeToOpenControlEvents(pedalNode, pedal, startid);

  return true;
}

/**
 * Write a rubato zone as an MEI line control event.
 *
 * Pianomania: the zone is emitted as <line type="pm-rubato-zone"> with
 * @startid/@endid bound to the anchor ChordRests. Page geometry follows
 * the octave export: pm:x1y1x2y2 for the full extent and pm:segments for
 * the per-system pieces.
 */

bool MeiExporter::writeRubatoZone(const RubatoZone *rubatoZone,
                                  const std::string &startid) {
  IF_ASSERT_FAILED(rubatoZone) { return false; }

  pugi::xml_node zoneNode = m_currentNode.append_child("line");
  zoneNode.append_attribute("xml:id") =
      this->getXmlIdFor(rubatoZone, 'z').c_str();
  zoneNode.append_attribute("type") = "pm-rubato-zone";
  zoneNode.append_attribute("pm:whole-measures") = "true";
  zoneNode.append_attribute("startid") = startid.c_str();

  if (!rubatoZone->segmentsEmpty()) {
    auto makePoint = [this](const PointF &pagePoint) {
      double x = pagePoint.x() / DPI;
      double y = toBottomLeftInches(pagePoint.y());
      return std::pair<double, double>(x, y);
    };

    const SpannerSegment *firstSegment = rubatoZone->frontSegment();
    const SpannerSegment *lastSegment = rubatoZone->backSegment();

    if (firstSegment && lastSegment) {
      auto [x1, y1] = makePoint(firstSegment->pagePos());
      PointF lastEndPos = lastSegment->pagePos() + lastSegment->pos2();
      auto [x2, y2] = makePoint(lastEndPos);

      std::string zoneXY = formatDecimalStr(x1, 3) + std::string(",") +
                           formatDecimalStr(y1, 3) + std::string(",") +
                           formatDecimalStr(x2, 3) + std::string(",") +
                           formatDecimalStr(y2, 3);
      zoneNode.append_attribute("pm:x1y1x2y2") = zoneXY.c_str();

      std::string segmentData;
      for (const SpannerSegment *seg : rubatoZone->spannerSegments()) {
        if (!seg) {
          continue;
        }

        PointF segStart = seg->pagePos();
        PointF segEnd = seg->pagePos() + seg->pos2();
        auto [sx, sy] = makePoint(segStart);
        auto [ex, ey] = makePoint(segEnd);

        if (!segmentData.empty()) {
          segmentData += ";";
        }

        segmentData += formatDecimalStr(sx, 3) + std::string(",") +
                       formatDecimalStr(sy, 3) + std::string(",") +
                       formatDecimalStr(ex, 3) + std::string(",") +
                       formatDecimalStr(ey, 3);
      }

      if (!segmentData.empty()) {
        zoneNode.append_attribute("pm:segments") = segmentData.c_str();
      }
    }
  }

  // Add the node to the map of open control events so @endid resolves.
  this->addNodeToOpenControlEvents(zoneNode, rubatoZone, startid);

  return true;
}

/**
 * Write a dance-show effect span (pyro, laser) as an MEI line control event.
 *
 * Pianomania: the span is emitted as <line type="pm-pyro-span"> or
 * <line type="pm-laser-span"> with @startid/@endid bound to the anchor
 * ChordRests through the generic spanner endpoint anchors. Page geometry
 * follows the rubato zone export: pm:x1y1x2y2 for the full extent and
 * pm:segments for the per-system pieces.
 */

bool MeiExporter::writeDanceShowSpan(const Spanner *span,
                                     const std::string &startid,
                                     const char *meiType, char xmlIdPrefix) {
  IF_ASSERT_FAILED(span) { return false; }

  const auto positionForTick = [this](const Fraction &tick, bool exclusiveEnd)
      -> std::optional<std::pair<int, double>> {
    if (!m_score || tick < Fraction(0, 1) ||
        (exclusiveEnd && tick <= Fraction(0, 1))) {
      return std::nullopt;
    }

    const Fraction measureLookupTick =
        exclusiveEnd ? tick - Fraction::eps() : tick;
    const Measure *measure = m_score->tick2measure(measureLookupTick);
    if (!measure) {
      return std::nullopt;
    }

    int measureIndex = 0;
    const Measure *indexedMeasure = m_score->firstMeasure();
    while (indexedMeasure && indexedMeasure != measure) {
      indexedMeasure = indexedMeasure->nextMeasure();
      ++measureIndex;
    }
    if (!indexedMeasure) {
      return std::nullopt;
    }

    const TimeSigFrac timeSig = measure->timesig();
    const double ticksPerBeat = timeSig.dUnitTicks();
    if (ticksPerBeat <= 0.0) {
      return std::nullopt;
    }
    const double beat =
        ((tick - measure->tick()).ticks() / ticksPerBeat) + 1.0;
    return std::make_pair(measureIndex, beat);
  };

  const auto startPosition = positionForTick(span->tick(), false);
  const auto endPosition = positionForTick(span->tick2(), true);
  if (!startPosition || !endPosition) {
    LOGE() << span->typeName() << " ticks " << span->tick().ticks() << ".."
           << span->tick2().ticks()
           << " has an invalid authoritative score-time position.";
    return false;
  }

  pugi::xml_node spanNode = m_currentNode.append_child("line");
  spanNode.append_attribute("xml:id") =
      this->getXmlIdFor(span, xmlIdPrefix).c_str();
  spanNode.append_attribute("type") = meiType;
  spanNode.append_attribute("startid") = startid.c_str();
  spanNode.append_attribute("pm:start-measure-index") = startPosition->first;
  spanNode.append_attribute("pm:start-beat") =
      formatBeatPosition(startPosition->second).c_str();
  spanNode.append_attribute("pm:end-measure-index") = endPosition->first;
  spanNode.append_attribute("pm:end-beat") =
      formatBeatPosition(endPosition->second).c_str();

  if (!span->segmentsEmpty()) {
    auto makePoint = [this](const PointF &pagePoint) {
      double x = pagePoint.x() / DPI;
      double y = toBottomLeftInches(pagePoint.y());
      return std::pair<double, double>(x, y);
    };

    const SpannerSegment *firstSegment = span->frontSegment();
    const SpannerSegment *lastSegment = span->backSegment();

    if (firstSegment && lastSegment) {
      auto [x1, y1] = makePoint(firstSegment->pagePos());
      PointF lastEndPos = lastSegment->pagePos() + lastSegment->pos2();
      auto [x2, y2] = makePoint(lastEndPos);

      std::string spanXY = formatDecimalStr(x1, 3) + std::string(",") +
                           formatDecimalStr(y1, 3) + std::string(",") +
                           formatDecimalStr(x2, 3) + std::string(",") +
                           formatDecimalStr(y2, 3);
      spanNode.append_attribute("pm:x1y1x2y2") = spanXY.c_str();

      std::string segmentData;
      for (const SpannerSegment *seg : span->spannerSegments()) {
        if (!seg) {
          continue;
        }

        PointF segStart = seg->pagePos();
        PointF segEnd = seg->pagePos() + seg->pos2();
        auto [sx, sy] = makePoint(segStart);
        auto [ex, ey] = makePoint(segEnd);

        if (!segmentData.empty()) {
          segmentData += ";";
        }

        segmentData += formatDecimalStr(sx, 3) + std::string(",") +
                       formatDecimalStr(sy, 3) + std::string(",") +
                       formatDecimalStr(ex, 3) + std::string(",") +
                       formatDecimalStr(ey, 3);
      }

      if (!segmentData.empty()) {
        spanNode.append_attribute("pm:segments") = segmentData.c_str();
      }
    }
  }

  // Add the node to the map of open control events so @endid resolves.
  this->addNodeToOpenControlEvents(spanNode, span, startid);

  return true;
}

/**
 * Write a repeatMark from a Jump.
 */

bool MeiExporter::writeRepeatMark(const Jump *jump, const Measure *measure) {
  IF_ASSERT_FAILED(jump && measure) { return false; }

  pugi::xml_node repeatMarkNode = m_currentNode.append_child();
  String text;
  libmei::RepeatMark meiRepeatMark = Convert::jumpToMEI(jump, text);

  if (text.size() > 0) {
    repeatMarkNode.text().set(text.toStdString().c_str());
  }

  meiRepeatMark.SetTstamp(0.0);
  std::string xmlId = this->getXmlIdFor(jump, 'r');
  meiRepeatMark.Write(repeatMarkNode, xmlId);

  appendCenteredPmPosition(repeatMarkNode, jump);
  repeatMarkNode.append_attribute("yOffset") =
      formatDecimalStr(getRepeatMarkYOffset(jump, measure), 1);

  // Currently not used - builds a post-processing list to be processing in
  // MeiExporter::addJumpToRepeatMarks
  // this->addToRepeatMarkList(static_cast<const TextBase*>(jump),
  // repeatMarkNode, xmlId);

  return true;
}

/**
 * Write a reh from a RehearsalMark.
 */

bool MeiExporter::writeRehearsalMark(const RehearsalMark *mark,
                                     const std::string &startid) {
  IF_ASSERT_FAILED(mark) { return false; }

  pugi::xml_node rehNode = m_currentNode.append_child();
  String text = mark->plainText();
  libmei::Reh meiReh;
  Convert::colorToMEI(mark, meiReh);

  if (text.size() > 0) {
    rehNode.text().set(text.toStdString().c_str());
  }

  meiReh.SetStartid(startid);

  std::string xmlId = this->getXmlIdFor(mark, 'r');
  meiReh.Write(rehNode, xmlId);

  appendCenteredPmPosition(rehNode, mark);

  return true;
}

/**
 * Write a repeatMark from a Marker.
 */

bool MeiExporter::writeRepeatMark(const Marker *marker,
                                  const Measure *measure) {
  IF_ASSERT_FAILED(marker && measure) { return false; }

  pugi::xml_node repeatMarkNode = m_currentNode.append_child();
  String text;
  libmei::RepeatMark meiRepeatMark = Convert::markerToMEI(marker, text);

  if (text.size() > 0) {
    repeatMarkNode.text().set(text.toStdString().c_str());
  }

  meiRepeatMark.SetTstamp(0.0);
  std::string xmlId = this->getXmlIdFor(marker, 'r');
  meiRepeatMark.Write(repeatMarkNode, xmlId);

  appendCenteredPmPosition(repeatMarkNode, marker);
  repeatMarkNode.append_attribute("yOffset") =
      formatDecimalStr(getRepeatMarkYOffset(marker, measure), 1);

  // Currently not used.
  // this->addToRepeatMarkList(dynamic_cast<const TextBase*>(marker),
  // repeatMarkNode, xmlId);

  return true;
}

std::vector<const Note *> MeiExporter::collectSlurNotes(const Slur *slur) const {
  std::vector<const Note *> coveredNotes;
  if (!slur) {
    return coveredNotes;
  }

  const ChordRest *startCR = const_cast<Slur *>(slur)->startCR();
  const ChordRest *endCR = const_cast<Slur *>(slur)->endCR();
  if (!startCR || !endCR) {
    return coveredNotes;
  }

  Segment *startSegment = startCR->segment();
  Segment *endSegment = endCR->segment();
  if (!startSegment || !endSegment) {
    return coveredNotes;
  }

  Fraction startTick = slur->tick();
  Fraction endTick = slur->tick2();

  track_idx_t trackStart = std::min(slur->track(), slur->effectiveTrack2());
  track_idx_t trackEnd = std::max(slur->track(), slur->effectiveTrack2());

  std::vector<track_idx_t> candidateTracks;
  candidateTracks.reserve(static_cast<size_t>((trackEnd - trackStart) + 1));
  for (track_idx_t track = trackStart; track <= trackEnd; ++track) {
    if (slur->elementAppliesToTrack(track)) {
      candidateTracks.push_back(track);
    }
  }

  if (candidateTracks.empty()) {
    candidateTracks.push_back(slur->track());
  }

  std::unordered_set<const Note *> seen;

  for (Segment *segment = startSegment; segment; segment = segment->next1()) {
    for (track_idx_t track : candidateTracks) {
      const EngravingItem *item = segment->element(track);
      const ChordRest *chordRest = dynamic_cast<const ChordRest *>(item);
      if (!chordRest) {
        continue;
      }

      Fraction chordTick = chordRest->tick();
      if (chordTick < startTick || chordTick > endTick) {
        continue;
      }

      const Chord *chord = dynamic_cast<const Chord *>(chordRest);
      if (!chord) {
        continue;
      }

      auto addChordNotes = [&](const Chord *sourceChord) {
        if (!sourceChord) {
          return;
        }
        Fraction sourceTick = sourceChord->tick();
        if (sourceTick < startTick || sourceTick > endTick) {
          return;
        }
        for (const Note *note : sourceChord->notes()) {
          if (!shouldExportNote(note)) {
            continue;
          }
          if (seen.insert(note).second) {
            coveredNotes.push_back(note);
          }
        }
      };

      addChordNotes(chord);

      const GraceNotesGroup &before = chord->graceNotesBefore();
      for (const Chord *graceChord : before) {
        addChordNotes(graceChord);
      }

      const GraceNotesGroup &after = chord->graceNotesAfter();
      for (const Chord *graceChord : after) {
        addChordNotes(graceChord);
      }
    }

    if (segment == endSegment) {
      break;
    }
  }

  return coveredNotes;
}

std::vector<const Note *> MeiExporter::collectHairpinCoveredNotes(
    const Hairpin *hairpin) const {
  std::vector<const Note *> coveredNotes;
  if (!hairpin || !m_score) {
    return coveredNotes;
  }

  const ChordRest *startCR = hairpin->findStartCR();
  const ChordRest *endCR = hairpin->findEndCR();
  const Chord *startChord =
      startCR && startCR->isChord() ? toChord(startCR) : nullptr;
  const Chord *endChord = endCR && endCR->isChord() ? toChord(endCR) : nullptr;

  Fraction startTick = hairpin->tick();
  Fraction endTick = hairpin->tick2();
  if (endTick <= startTick) {
    return coveredNotes;
  }

  Segment *startSegment = hairpin->startSegment();
  Segment *endSegment = hairpin->endSegment();
  if (!startSegment || !endSegment) {
    return coveredNotes;
  }

  std::unordered_set<const Note *> excluded;
  if (startChord) {
    for (const Note *note : startChord->notes()) {
      if (note) {
        excluded.insert(note);
      }
    }
  }

  if (endChord) {
    for (const Note *note : endChord->notes()) {
      if (note) {
        excluded.insert(note);
      }
    }
  }

  auto addStaffIfNeeded = [](std::vector<staff_idx_t> &staves,
                              staff_idx_t staff) {
    if (staff == muse::nidx) {
      return;
    }
    if (std::find(staves.begin(), staves.end(), staff) == staves.end()) {
      staves.push_back(staff);
    }
  };

  staff_idx_t startStaff = track2staff(hairpin->track());
  staff_idx_t endStaff = track2staff(hairpin->effectiveTrack2());
  if (startStaff == muse::nidx && endStaff == muse::nidx) {
    return coveredNotes;
  }

  staff_idx_t topStaff = startStaff;
  staff_idx_t bottomStaff = endStaff;
  if (startStaff == muse::nidx) {
    topStaff = bottomStaff = endStaff;
  } else if (endStaff == muse::nidx) {
    topStaff = bottomStaff = startStaff;
  } else {
    topStaff = std::min(startStaff, endStaff);
    bottomStaff = std::max(startStaff, endStaff);
  }

  std::vector<staff_idx_t> targetStaves;
  if (hairpin->centerBetweenStaves() == AutoOnOff::ON ||
      hairpin->direction() == DirectionV::AUTO) {
    addStaffIfNeeded(targetStaves, topStaff);
    addStaffIfNeeded(targetStaves, bottomStaff);
  } else if (hairpin->direction() == DirectionV::UP) {
    addStaffIfNeeded(targetStaves, topStaff);
  } else if (hairpin->direction() == DirectionV::DOWN) {
    addStaffIfNeeded(targetStaves, bottomStaff);
  } else {
    addStaffIfNeeded(targetStaves, topStaff);
    addStaffIfNeeded(targetStaves, bottomStaff);
  }

  if (targetStaves.empty()) {
    return coveredNotes;
  }

  track_idx_t trackCount = m_score->ntracks();
  if (trackCount == 0) {
    return coveredNotes;
  }

  std::unordered_set<const Note *> seen;

  for (Segment *segment = startSegment; segment; segment = segment->next1()) {
    Fraction segmentTick = segment->tick();
    if (segmentTick <= startTick || segmentTick >= endTick) {
      if (segment == endSegment) {
        break;
      }
      continue;
    }

    for (staff_idx_t staff : targetStaves) {
      track_idx_t firstTrack = staff2track(staff);
      for (int voice = 0; voice < static_cast<int>(VOICES); ++voice) {
        track_idx_t track = firstTrack + voice;
        if (track >= trackCount) {
          continue;
        }

        if (!hairpin->elementAppliesToTrack(track)) {
          continue;
        }

        const EngravingItem *item = segment->element(track);
        const Chord *chord = dynamic_cast<const Chord *>(item);
        if (!chord || chord->isGrace()) {
          continue;
        }

        for (const Note *note : chord->notes()) {
          if (!shouldExportNote(note)) {
            continue;
          }
          if (excluded.find(note) != excluded.end()) {
            continue;
          }
          if (seen.insert(note).second) {
            coveredNotes.push_back(note);
          }
        }
      }
    }

    if (segment == endSegment) {
      break;
    }
  }

  return coveredNotes;
}

MeiExporter::TieContextNoteLists
MeiExporter::collectTieContextNotes(const Tie *tie) const {
  TieContextNoteLists context;
  if (!tie) {
    return context;
  }

  const Note *startNote = tie->startNote();
  const Note *endNote = tie->endNote();
  if (!startNote || !endNote) {
    return context;
  }

  const Chord *startChord = startNote->chord();
  const Chord *endChord = endNote->chord();
  if (!startChord || !endChord) {
    return context;
  }

  auto isCandidateNote = [&](const Note *candidate) {
    if (!shouldExportNote(candidate) || candidate == startNote ||
        candidate == endNote) {
      return false;
    }

    // Ignore continuation notes (those tied from an earlier attack)
    if (candidate->tieBack()) {
      return false;
    }

    return true;
  };

  Segment *startSegment = startChord->segment();
  Segment *endSegment = endChord->segment();
  if (!startSegment || !endSegment) {
    return context;
  }

  Fraction startTick = startChord->tick();
  Fraction endTick = endChord->tick();
  if (endTick <= startTick) {
    return context;
  }

  const Measure *endMeasure = endChord->measure();
  if (!endMeasure) {
    return context;
  }

  TimeSigFrac timeSig = endMeasure->timesig();
  int ticksPerBeat = timeSig.dUnitTicks();
  Fraction terminalBeatStart = endTick;
  Fraction terminalBeatEnd = endTick;

  if (ticksPerBeat > 0) {
    Fraction measureTick = endMeasure->tick();
    Fraction offset = endTick - measureTick;
    int beatIndex = offset.ticks() / ticksPerBeat;
    if (beatIndex < 0) {
      beatIndex = 0;
    }

    Fraction beatStartOffset = Fraction::fromTicks(beatIndex * ticksPerBeat);
    Fraction beatEndOffset = Fraction::fromTicks((beatIndex + 1) * ticksPerBeat);
    terminalBeatStart = measureTick + beatStartOffset;
    terminalBeatEnd = measureTick + beatEndOffset;
  } else {
    terminalBeatEnd = terminalBeatStart + Fraction::eps();
  }

  track_idx_t trackCount = m_score ? m_score->ntracks() : 0;
  if (trackCount == 0) {
    return context;
  }

  std::unordered_set<const Note *> seenCovered;
  std::unordered_set<const Note *> seenTerminal;

  for (Segment *segment = startSegment; segment; segment = segment->next1()) {
    for (track_idx_t track = 0; track < trackCount; ++track) {
      const EngravingItem *item = segment->element(track);
      const Chord *chord = dynamic_cast<const Chord *>(item);
      if (!chord || chord->isGrace()) {
        continue;
      }

      Fraction chordTick = chord->tick();
      for (const Note *note : chord->notes()) {
        if (!isCandidateNote(note)) {
          continue;
        }

        if (chordTick >= terminalBeatStart && chordTick < terminalBeatEnd) {
          if (seenTerminal.insert(note).second) {
            context.terminal.push_back(note);
          }
        } else if (chordTick > startTick && chordTick < endTick) {
          if (seenCovered.insert(note).second) {
            context.covered.push_back(note);
          }
        }
      }
    }

    if (segment == endSegment) {
      break;
    }
  }

  if (context.covered.empty() && context.terminal.empty()) {
    std::unordered_set<const Note *> seenPost;
    bool haveFirstTick = false;
    Fraction firstTick;

    for (Segment *segment = endSegment; segment; segment = segment->next1()) {
      Fraction segmentTick = segment->tick();
      if (segmentTick <= endTick) {
        continue;
      }

      if (!haveFirstTick) {
        firstTick = segmentTick;
        haveFirstTick = true;
      } else if (segmentTick > firstTick) {
        break;
      }

      for (track_idx_t track = 0; track < trackCount; ++track) {
        const EngravingItem *item = segment->element(track);
        const Chord *chord = dynamic_cast<const Chord *>(item);
        if (!chord || chord->isGrace()) {
          continue;
        }

        Fraction chordTick = chord->tick();
        if (chordTick <= endTick) {
          continue;
        }

        for (const Note *note : chord->notes()) {
          if (!isCandidateNote(note)) {
            continue;
          }

          if (seenPost.insert(note).second) {
            context.postTerminal.push_back(note);
          }
        }
      }
    }
  }

  return context;
}

/**
 * Write a slur.
 */

bool MeiExporter::writeSlur(const Slur *slur, const std::string &startid) {
  IF_ASSERT_FAILED(slur) { return false; }

  pugi::xml_node slurNode = m_currentNode.append_child();
  libmei::Slur meiSlur = Convert::slurToMEI(slur);
  meiSlur.SetStartid(startid);

  meiSlur.Write(slurNode, this->getXmlIdFor(slur, 's'));

  auto makePoint = [this](const PointF &pagePoint) {
    double x = pagePoint.x() / DPI;
    double y = toBottomLeftInches(pagePoint.y());
    return std::pair<double, double>(x, y);
  };

  auto gripPos = [](const SlurTieSegment *segment,
                    Grip grip) -> std::optional<PointF> {
    if (!segment) {
      return std::nullopt;
    }
    PointF segmentPos = segment->pagePos();
    return segmentPos + segment->ups(grip).pos();
  };

  PointF startPos = slur->startElement()->pagePos();
  PointF endPos = slur->endElement()->pagePos();

  if (!slur->segmentsEmpty()) {
    if (const SlurSegment *firstSegment = slur->frontSegment()) {
      if (auto absolute = gripPos(firstSegment, Grip::START)) {
        startPos = *absolute;
      }
    }
    if (const SlurSegment *lastSegment = slur->backSegment()) {
      if (auto absolute = gripPos(lastSegment, Grip::END)) {
        endPos = *absolute;
      }
    }
  }

  auto [x1, y1] = makePoint(startPos);
  auto [x2, y2] = makePoint(endPos);
  std::string slurXY = formatDecimalStr(x1, 3) + std::string(",") +
                       formatDecimalStr(y1, 3) + std::string(",") +
                       formatDecimalStr(x2, 3) + std::string(",") +
                       formatDecimalStr(y2, 3);
  slurNode.append_attribute("pm:x1y1x2y2") = slurXY.c_str();

  if (!slur->segmentsEmpty()) {
    std::string bezierData;
    for (size_t i = 0; i < slur->nsegments(); ++i) {
      const SlurSegment *segment = slur->segmentAt(static_cast<int>(i));
      if (!segment) {
        continue;
      }

      auto startGrip = gripPos(segment, Grip::START);
      auto bezier1Grip = gripPos(segment, Grip::BEZIER1);
      auto bezier2Grip = gripPos(segment, Grip::BEZIER2);
      auto endGrip = gripPos(segment, Grip::END);

      if (!startGrip || !bezier1Grip || !bezier2Grip || !endGrip) {
        continue;
      }

      auto [sx, sy] = makePoint(*startGrip);
      auto [c1x, c1y] = makePoint(*bezier1Grip);
      auto [c2x, c2y] = makePoint(*bezier2Grip);
      auto [ex, ey] = makePoint(*endGrip);

      if (!bezierData.empty()) {
        bezierData += ";";
      }

      bezierData += formatDecimalStr(sx, 3) + std::string(",") +
                    formatDecimalStr(sy, 3) + std::string(",") +
                    formatDecimalStr(c1x, 3) + std::string(",") +
                    formatDecimalStr(c1y, 3) + std::string(",") +
                    formatDecimalStr(c2x, 3) + std::string(",") +
                    formatDecimalStr(c2y, 3) + std::string(",") +
                    formatDecimalStr(ex, 3) + std::string(",") +
                    formatDecimalStr(ey, 3);
    }

    if (!bezierData.empty()) {
      slurNode.append_attribute("pm:bezier") = bezierData.c_str();
    }
  }

  std::vector<const Note *> coveredNotes = collectSlurNotes(slur);
  std::string uuidList;
  for (const Note *note : coveredNotes) {
    if (!shouldExportNote(note)) {
      continue;
    }

    std::string noteId;
    auto cachedId = m_noteXmlIdCache.find(note);
    if (cachedId != m_noteXmlIdCache.end()) {
      noteId = cachedId->second;
    } else {
      noteId = getXmlIdFor(note, 'n');
      if (!noteId.empty()) {
        m_noteXmlIdCache[note] = noteId;
      }
    }
    if (noteId.empty()) {
      continue;
    }
    if (!uuidList.empty()) {
      uuidList += ' ';
    }
    uuidList += noteId;
  }

  if (!uuidList.empty()) {
    slurNode.append_attribute("pm:coveredUuids") = uuidList.c_str();
  }

  // Add the node to the map of open control events
  this->addNodeToOpenControlEvents(slurNode, slur, startid);

  return true;
}

/**
 * Write a tempo and its text content.
 */

bool MeiExporter::writeTempo(const TempoText *tempoText,
                             const std::string &startid) {
  IF_ASSERT_FAILED(tempoText) { return false; }

  StringList meiLines;

  pugi::xml_node tempoNode = m_currentNode.append_child();
  libmei::Tempo meiTempo = Convert::tempoToMEI(tempoText, meiLines);
  if (tempoText->tick() == tempoText->measure()->tick()) {
    double tstamp = Convert::tstampFromFraction(
        tempoText->tick() - tempoText->measure()->tick(),
        tempoText->measure()->timesig());
    meiTempo.SetTstamp(tstamp);
  } else {
    meiTempo.SetStartid(startid);
  }
  meiTempo.Write(tempoNode, this->getXmlIdFor(tempoText, 't'));

  appendCenteredPmPosition(tempoNode, tempoText);

  this->writeLinesWithSMuFL(tempoNode, meiLines);

  return true;
}

/**
 * Write a tie.
 */

bool MeiExporter::writeTie(const Tie *tie, const std::string &startid) {
  IF_ASSERT_FAILED(tie) { return false; }

  pugi::xml_node tieNode = m_currentNode.append_child();
  libmei::Tie meiTie = Convert::tieToMEI(tie);
  meiTie.SetStartid(startid);

  meiTie.Write(tieNode,
               this->getXmlIdFor(tie, tie->isLaissezVib() ? 'l' : 't'));

  // Change open ties by simply adjusting the element name
  if (tie->isLaissezVib()) {
    tieNode.set_name("lv");
  }

  auto makePoint = [this](const PointF &pagePoint) {
    double x = pagePoint.x() / DPI;
    double y = toBottomLeftInches(pagePoint.y());
    return std::pair<double, double>(x, y);
  };

  auto gripPos = [](const SlurTieSegment *segment,
                    Grip grip) -> std::optional<PointF> {
    if (!segment) {
      return std::nullopt;
    }
    PointF segmentPos = segment->pagePos();
    return segmentPos + segment->ups(grip).pos();
  };

  std::optional<PointF> startPos;
  std::optional<PointF> endPos;

  if (const Note *startNote = tie->startNote()) {
    startPos = startNote->pagePos();
  }
  if (const Note *endNote = tie->endNote()) {
    endPos = endNote->pagePos();
  }

  if (!tie->segmentsEmpty()) {
    if (const TieSegment *firstSegment = tie->frontSegment()) {
      if (auto absolute = gripPos(firstSegment, Grip::START)) {
        startPos = *absolute;
      }
    }
    if (const TieSegment *lastSegment = tie->backSegment()) {
      if (auto absolute = gripPos(lastSegment, Grip::END)) {
        endPos = *absolute;
      }
    }
  }

  if (startPos && endPos) {
    auto [x1, y1] = makePoint(*startPos);
    auto [x2, y2] = makePoint(*endPos);
    std::string tieXY = formatDecimalStr(x1, 3) + std::string(",") +
                        formatDecimalStr(y1, 3) + std::string(",") +
                        formatDecimalStr(x2, 3) + std::string(",") +
                        formatDecimalStr(y2, 3);
    tieNode.append_attribute("pm:x1y1x2y2") = tieXY.c_str();
  }

  if (!tie->segmentsEmpty()) {
    std::string bezierData;
    for (size_t i = 0; i < tie->nsegments(); ++i) {
      const TieSegment *segment = tie->segmentAt(static_cast<int>(i));
      if (!segment) {
        continue;
      }

      auto startGrip = gripPos(segment, Grip::START);
      auto bezier1Grip = gripPos(segment, Grip::BEZIER1);
      auto bezier2Grip = gripPos(segment, Grip::BEZIER2);
      auto endGrip = gripPos(segment, Grip::END);

      if (!startGrip || !bezier1Grip || !bezier2Grip || !endGrip) {
        continue;
      }

      auto [sx, sy] = makePoint(*startGrip);
      auto [c1x, c1y] = makePoint(*bezier1Grip);
      auto [c2x, c2y] = makePoint(*bezier2Grip);
      auto [ex, ey] = makePoint(*endGrip);

      if (!bezierData.empty()) {
        bezierData += ";";
      }

      bezierData += formatDecimalStr(sx, 3) + std::string(",") +
                    formatDecimalStr(sy, 3) + std::string(",") +
                    formatDecimalStr(c1x, 3) + std::string(",") +
                    formatDecimalStr(c1y, 3) + std::string(",") +
                    formatDecimalStr(c2x, 3) + std::string(",") +
                    formatDecimalStr(c2y, 3) + std::string(",") +
                    formatDecimalStr(ex, 3) + std::string(",") +
                    formatDecimalStr(ey, 3);
    }

    if (!bezierData.empty()) {
      tieNode.append_attribute("pm:bezier") = bezierData.c_str();
    }
  }

  TieContextNoteLists tieContext = collectTieContextNotes(tie);

  auto buildNoteIdList = [this](const std::vector<const Note *> &notes) {
    std::string idList;
    for (const Note *note : notes) {
      if (!shouldExportNote(note)) {
        continue;
      }

      std::string noteId;
      auto cachedId = m_noteXmlIdCache.find(note);
      if (cachedId != m_noteXmlIdCache.end()) {
        noteId = cachedId->second;
      } else {
        noteId = getXmlIdFor(note, 'n');
        if (!noteId.empty()) {
          m_noteXmlIdCache[note] = noteId;
        }
      }

      if (noteId.empty()) {
        continue;
      }

      if (!idList.empty()) {
        idList += ' ';
      }
      idList += noteId;
    }
    return idList;
  };

  if (!tieContext.covered.empty()) {
    std::string coveredIds = buildNoteIdList(tieContext.covered);
    if (!coveredIds.empty()) {
      tieNode.append_attribute("pm:covered-ids") = coveredIds.c_str();
    }
  }

  if (!tieContext.terminal.empty()) {
    std::string terminalIds = buildNoteIdList(tieContext.terminal);
    if (!terminalIds.empty()) {
      tieNode.append_attribute("pm:terminal-ids") = terminalIds.c_str();
    }
  }

  if (tieContext.covered.empty() && tieContext.terminal.empty() &&
      !tieContext.postTerminal.empty()) {
    std::string postTerminalIds = buildNoteIdList(tieContext.postTerminal);
    if (!postTerminalIds.empty()) {
      tieNode.append_attribute("pm:post-terminal-ids") =
          postTerminalIds.c_str();
    }
  }

  // Add the node to the map of open control events
  this->addNodeToOpenControlEvents(tieNode, tie, startid);

  return true;
}

/**
 * Write a glissando with its exact note anchor.
 */

bool MeiExporter::writeGliss(const Glissando *gliss,
                             const std::string &startid) {
  IF_ASSERT_FAILED(gliss) { return false; }

  pugi::xml_node glissNode = m_currentNode.append_child();
  libmei::Gliss meiGliss = Convert::glissToMEI(gliss);
  meiGliss.SetStartid(startid);
  meiGliss.Write(glissNode, this->getXmlIdFor(gliss, 'g'));
  this->addNodeToOpenControlEvents(glissNode, gliss, startid);
  return true;
}

/**
 * Write a trill.
 */

bool MeiExporter::writeTrill(const Trill *trill, const std::string &startid) {
  IF_ASSERT_FAILED(trill) { return false; }

  const Ornament *ornament = trill->ornament();
  pugi::xml_node trillNode = m_currentNode.append_child();
  libmei::Trill meiTrill = Convert::trillToMEI(ornament);
  Convert::colorlineToMEI(trill, meiTrill);
  meiTrill.SetExtender(libmei::BOOLEAN_true);
  meiTrill.SetStartid(startid);

  meiTrill.Write(trillNode, this->getXmlIdFor(trill, 't'));

  if (ornament) {
    double yOffset = getOrnamentYOffset(ornament);
    trillNode.append_attribute("yOffset") = formatDecimalStr(yOffset, 1).c_str();
  }

  // Add the node to the map of open control events
  this->addNodeToOpenControlEvents(trillNode, trill, startid);

  return true;
}

//---------------------------------------------------------
// write MEI attribute classes
//---------------------------------------------------------

/**
 * Write the beam attributes for a ChordRest (i.e., chord, note, rest or space).
 * Uses the `@type` attribute for storing MuseScore beaming added flags.
 */

bool MeiExporter::writeBeamTypeAtt(const ChordRest *chordRest,
                                   libmei::AttTyped &typeAtt) {
  IF_ASSERT_FAILED(chordRest) { return false; }

  // Make sure we do not add a @type for notes / rests longer the 8th with a
  // hanging beam flag
  if (int(chordRest->durationType().type()) < int(DurationType::V_EIGHTH)) {
    return true;
  }

  switch (chordRest->beamMode()) {
  // BeamMode::BEGIN16 and BEGIN32 is handled in MeiExporter::writeBeam, which
  // will add MEI a @breaksec to the previous element This is BeamMode in
  // MuseScore is on the first note _after_ the break, whereas it is on the last
  // note _before_ it in MEI.
  case (BeamMode::BEGIN):
  case (BeamMode::MID):
  case (BeamMode::NONE):
    typeAtt.SetType(
        Convert::beamToMEI(chordRest->beamMode(), BEAM_ELEMENT_TYPE));
    break;
  default:
    break;
  }

  return true;
}

/**
 * Write the cross-staff attribute (@staff) for a ChordRest (i.e., chord, note,
 * rest or space).
 */

bool MeiExporter::writeStaffIdentAtt(const ChordRest *chordRest,
                                     const Staff *staff,
                                     libmei::AttStaffIdent &staffIdentAtt) {
  if (chordRest->staffMove() != 0) {
    staff_idx_t staffN = staff->idx() + chordRest->staffMove() + 1;
    libmei::xsdPositiveInteger_List staffNs;
    staffNs.push_back(static_cast<int>(staffN));
    staffIdentAtt.SetStaff(staffNs);
  }

  return true;
}

/**
 * Write the stem attributes for a Chord (i.e., chord or note).
 */

bool MeiExporter::writeStemAtt(const Chord *chord, libmei::AttStems &stemsAtt) {
  IF_ASSERT_FAILED(chord) { return false; }

  // Use the computed/rendered stem direction, not the user-set property
  auto computedDir =
      chord->up() ? engraving::DirectionV::UP : engraving::DirectionV::DOWN;
  auto [meiStemDir, meiStemLen] =
      Convert::stemToMEI(computedDir, chord->noStem());
  stemsAtt.SetStemDir(meiStemDir);
  stemsAtt.SetStemLen(meiStemLen);

  if (chord->tremoloChordType() == TremoloChordType::TremoloSingle) {
    stemsAtt.SetStemMod(Convert::stemModToMEI(chord->tremoloSingleChord()));
  }

  return true;
}

bool MeiExporter::isCurrentNode(const libmei::Element &element) {
  return element.m_name == std::string(m_currentNode.name());
}

/**
 * When writing a measure, find voltas (endings) spanning over it.
 * This will then be use to check if the measure is the beginning or the end or
 * a volta.
 */

std::list<const Volta *>
MeiExporter::findVoltasInMeasure(const Measure *measure) {
  std::list<const Volta *> voltas;
  auto spanners = m_score->spannerMap().findOverlapping(
      measure->tick().ticks(), measure->endTick().ticks());
  for (auto interval : spanners) {
    Spanner *spanner = interval.value;
    if (spanner && spanner->isVolta()) {
      voltas.push_back(toVolta(spanner));
    }
  }
  return voltas;
}

/**
 * Go through the list of annotations pointing to the ChordRest and map their
 * element to the xmlId. When writing the annotation (i.e., control events) the
 * map is used to generate the `@startid` of the control event. The value in the
 * map already contains the `#` Also add Breath attached to a ChordRest, which
 * are not represented as annotations but di
 */

void MeiExporter::fillControlEventMap(const std::string &xmlId,
                                      const ChordRest *chordRest,
                                      bool includeSegmentAnnotations) {
  IF_ASSERT_FAILED(chordRest) { return; }

  track_idx_t trackIdx = chordRest->track();

  if (includeSegmentAnnotations) {
    this->flushPendingControlEvents(xmlId, trackIdx);

    for (const EngravingItem *element : chordRest->segment()->annotations()) {
      if (element->track() == trackIdx) {
        m_startingControlEventList.push_back(
            std::make_pair(element, "#" + xmlId));
      }
    }
  }
  // Breath a handled differently
  const Breath *breath = chordRest->hasBreathMark();
  if (breath) {
    m_startingControlEventList.push_back(std::make_pair(breath, "#" + xmlId));
  }

  for (const auto &[rubatoZone, anchor] : m_rubatoZoneStartAnchors) {
    if (anchor == chordRest) {
      m_startingControlEventList.push_back(
          std::make_pair(rubatoZone, "#" + xmlId));
    }
  }
  for (const auto &[rubatoZone, anchor] : m_rubatoZoneEndAnchors) {
    if (anchor == chordRest) {
      m_endingControlEventMap[rubatoZone] = "#" + xmlId;
    }
  }

  for (const auto &[danceShowSpan, anchor] : m_danceShowStartAnchors) {
    if (anchor == chordRest) {
      m_startingControlEventList.push_back(
          std::make_pair(danceShowSpan, "#" + xmlId));
    }
  }
  for (const auto &[danceShowSpan, anchor] : m_danceShowEndAnchors) {
    if (anchor == chordRest) {
      m_endingControlEventMap[danceShowSpan] = "#" + xmlId;
    }
  }

  const auto startRange = m_spannerStartAnchors.equal_range(chordRest);
  for (auto it = startRange.first; it != startRange.second; ++it) {
    m_startingControlEventList.push_back(
        std::make_pair(it->second, "#" + xmlId));
  }
  const auto endRange = m_spannerEndAnchors.equal_range(chordRest);
  for (auto it = endRange.first; it != endRange.second; ++it) {
    m_endingControlEventMap[it->second] = "#" + xmlId;
  }
  // For chords only
  if (chordRest->isChord()) {
    const Chord *chord = toChord(chordRest);
    // Ornaments and laissez vibrer
    for (const Articulation *articulation : chord->articulations()) {
      if (this->isLaissezVibrer(articulation->symId())) {
        m_startingControlEventList.push_back(
            std::make_pair(articulation, "#" + xmlId));
      } else if (articulation->isOrnament()) {
        m_startingControlEventList.push_back(
            std::make_pair(articulation, "#" + xmlId));
      }
    }
    // Arpeggio
    const Arpeggio *arpeggio = chord->arpeggio();
    if (arpeggio) {
      m_startingControlEventList.push_back(
          std::make_pair(arpeggio, "#" + xmlId));
      // The arpeggio is spanning to a lower staff
      if (arpeggio->span() > 1) {
        // We need to retrieve the chord it is spanning to
        track_idx_t bottomTrack = arpeggio->track() + (arpeggio->span() - 1);
        const EngravingItem *element = chord->segment()->element(bottomTrack);
        // We do not know the xml:id of the chord yet, keep it in a map
        if (element && element->isChord()) {
          m_arpegPlistMap[toChord(element)] = arpeggio;
        }
      }
    }
    // This is the lower chord of a spanning arpeggio - we can now move it to
    // the map with the chord xml:id
    if (m_arpegPlistMap.count(chord)) {
      m_plistMap[m_arpegPlistMap.at(chord)] = "#" + xmlId;
      m_arpegPlistMap.erase(chord);
    }
  }
}

void MeiExporter::collectPendingControlEvents(const ChordRest *chordRest) {
  IF_ASSERT_FAILED(chordRest) { return; }

  track_idx_t trackIdx = chordRest->track();

  for (const EngravingItem *element : chordRest->segment()->annotations()) {
    if (element->track() == trackIdx) {
      m_pendingControlEventList.push_back(std::make_pair(trackIdx, element));
    }
  }
}

void MeiExporter::flushPendingControlEvents(const std::string &xmlId,
                                            track_idx_t trackIdx) {
  for (auto it = m_pendingControlEventList.begin();
       it != m_pendingControlEventList.end();) {
    if (it->first != trackIdx) {
      ++it;
      continue;
    }

    m_startingControlEventList.push_back(
        std::make_pair(it->second, "#" + xmlId));
    it = m_pendingControlEventList.erase(it);
  }
}

/**
 * Retrieve the `@startid` for a control event.
 * The map has been filled previously by MeiExporter::addToStartIdMap.
 */

std::string MeiExporter::findStartIdFor(const EngravingItem *item) {
  std::string xmlId;

  auto result = std::find_if(
      m_startingControlEventList.begin(), m_startingControlEventList.end(),
      [item](const auto &entry) { return entry.first == item; });

  if (result != m_startingControlEventList.end()) {
    xmlId = result->second;
  }
  return xmlId;
}

/**
 * Keep a list of Mei::Exporter::RepeatMark for post-processing jumps.
 * This code is currently not used.
 * During the post-processing, if a m_jumpToXmlId can be determined, it will be
 * added to the XML m_node. See MeiExporter::addJumpToRepeatMarks
 */

void MeiExporter::addToRepeatMarkList(const EngravingItem *repeatMark,
                                      pugi::xml_node node,
                                      const std::string &xmlId) {
  IF_ASSERT_FAILED(repeatMark) { return; }

  m_repeatMarks.push_back(MeiExporter::RepeatMark());
  RepeatMark &repeatMarkItem = m_repeatMarks.back();
  repeatMarkItem.m_repeatMark = repeatMark;
  repeatMarkItem.m_node = node;
  repeatMarkItem.m_xmlId = xmlId;
}

/**
 * Add `@jumpto` to repeatMark MEI elements by unfolding MuseScore jumps.
 * This code is currently unused because `@jumpto` is not available in MEI.
 * The principle is to post-process the list of m_repeatMarks.
 * For each mark, we lookup the `@jumpto` xmlId, which can then be written to
 * the repeatMark node
 */

void MeiExporter::addJumpToRepeatMarks() {
  if (m_repeatMarks.size() < 1) {
    return;
  }

  for (RepeatMark &item : m_repeatMarks) {
    if (item.m_repeatMark->isJump()) {
      // For each jump, lookup the Marker that has the label matching the jumpTo
      const Jump *jump = toJump(item.m_repeatMark);
      item.m_jumptToLabel = jump->jumpTo().toStdString();
      auto jumpTo = std::find_if(
          m_repeatMarks.begin(), m_repeatMarks.end(), [jump](RepeatMark &item) {
            if (!item.m_repeatMark->isMarker()) {
              return false;
            }
            const Marker *marker = toMarker(item.m_repeatMark);
            // Found it
            return marker->label() == jump->jumpTo();
          });
      if (jumpTo != m_repeatMarks.end()) {
        // This is the xmlId we need to add to the (jump) repeatMark
        item.m_jumpToXmlId = jumpTo->m_xmlId;
      }

      // Try to see if we have a playUntil Marker and a continueAt Marker
      auto playUntil = std::find_if(
          m_repeatMarks.begin(), m_repeatMarks.end(), [jump](RepeatMark &item) {
            if (!item.m_repeatMark->isMarker()) {
              return false;
            }
            const Marker *marker = toMarker(item.m_repeatMark);
            return marker->label() == jump->playUntil();
          });
      auto continueAt = std::find_if(
          m_repeatMarks.begin(), m_repeatMarks.end(), [jump](RepeatMark &item) {
            if (!item.m_repeatMark->isMarker()) {
              return false;
            }
            const Marker *marker = toMarker(item.m_repeatMark);
            return marker->label() == jump->continueAt();
          });
      // If yes, make the playUntil repeatMark jump to the continueAt
      if (playUntil != m_repeatMarks.end() &&
          continueAt != m_repeatMarks.end()) {
        playUntil->m_jumpToXmlId = continueAt->m_xmlId;
      }
    }
  }
  // Add the jumpto attribute for all the repeatMarks for which we filled a
  // jumpToXmlId
  for (RepeatMark &item : m_repeatMarks) {
    if (item.m_jumpToXmlId.size() > 0) {
      item.m_node.append_attribute("jumpto") = item.m_jumpToXmlId.c_str();
    }
  }
}

/**
 * Check if the Segment (barLineEnd type) has a Fermata annotation for the given
 * track. Add the Fermata to the m_tstampControlEventMap with the appropriate
 * @staff and @tstamp values.
 */

bool MeiExporter::addFermataToMap(const track_idx_t track,
                                  const Segment *segment,
                                  const Measure *measure) {
  IF_ASSERT_FAILED(segment) { return false; }

  for (const auto &annotation : segment->annotations()) {
    if (annotation->isFermata() && track == annotation->track()) {
      staff_idx_t staffN = track2staff(track) + 1;
      libmei::xsdPositiveInteger_List staffNs;
      staffNs.push_back(static_cast<int>(staffN));
      double tstamp =
          Convert::tstampFromFraction(measure->ticks(), measure->timesig());
      m_tstampControlEventMap.push_back(std::make_pair(
          toFermata(annotation), std::make_pair(staffNs, tstamp)));
    }
  }

  return true;
}

/**
 * Return true if the node name matches the name parameter.
 */

bool MeiExporter::isNode(pugi::xml_node node, const String &name) {
  if (!node) {
    return false;
  }

  String nodeName = String(node.name());
  return nodeName == name;
}

/**
 * Return the last element in node that is a ChordRest (chord, note, or rest)
 */

pugi::xml_node MeiExporter::getLastChordRest(pugi::xml_node node) {
  pugi::xml_node chordRest;

  for (pugi::xml_node child : node.children()) {
    if (this->isNode(child, u"chord") || this->isNode(child, u"note") ||
        this->isNode(child, u"rest")) {
      chordRest = child;
    }
  }
  return chordRest;
}

/**
 * Add a spanner to the map of open control events to which a @endid needs to be
 * added. For spanners starting and ending on the same element (hairpin,
 * ottava), the @endid is added directly together with a @dur
 */

void MeiExporter::addNodeToOpenControlEvents(pugi::xml_node node,
                                             const Spanner *spanner,
                                             const std::string &startid) {
  if (spanner->isRubatoZone() || spanner->isPyroSpan() ||
      spanner->isLaserSpan()) {
    // These bind @endid through their dedicated anchor maps, so the
    // @endid always resolves later even when the start and end elements
    // coincide.
    m_openControlEventMap[spanner] = node;
    return;
  }
  if (spanner->startElement() &&
      (spanner->startElement() == spanner->endElement())) {
    // Add a @endid
    libmei::InstStartEndId startEndId;
    startEndId.SetEndid(startid);
    startEndId.WriteStartEndId(node);
    // Add a @dur
    if (spanner->startElement()->isChordRest()) {
      const ChordRest *startCR = toChordRest(spanner->startElement());
      libmei::InstDurationLog durationLog;
      durationLog.SetDur(Convert::durToMEI(startCR->durationType().type()));
      durationLog.WriteDurationLog(node);
    }
  } else {
    m_openControlEventMap[spanner] = node;
  }
}

/**
 * Go trough the list of control event maps and add @endid when the end element
 * has be written.
 */

void MeiExporter::addEndidToControlEvents() {
  std::list<const EngravingItem *> closedEvents;
  std::list<const EngravingItem *> closedPlists;

  // Go through the list of open control events and see if the end element has
  // been written
  for (auto controlEvent : m_openControlEventMap) {
    // Convenience variable
    const EngravingItem *item = controlEvent.first;
    // Check if we have both:
    // * the end @xml:id (in m_endingControlEventMap)
    // * the control event node (in m_openControlEventMap)
    if (m_endingControlEventMap.count(item) &&
        m_openControlEventMap.count(item)) {
      // Create an attribute class instance to add the @endid
      libmei::InstStartEndId startEndId;
      startEndId.SetEndid(m_endingControlEventMap.at(item));
      startEndId.WriteStartEndId(m_openControlEventMap.at(item));
      // Add it to the list of closed events we can remove from the maps (below)
      closedEvents.push_back(item);
    }
    if (m_plistMap.count(item) && m_openControlEventMap.count(item)) {
      // Create an attribute class instance to add the @plist
      libmei::InstPlist plist;
      plist.SetPlist({m_plistMap.at(item)});
      plist.WritePlist(m_openControlEventMap.at(item));
      // Add it to the list of closed plists we can remove from the maps (below)
      closedPlists.push_back(item);
    }
  }

  for (auto item : closedEvents) {
    m_openControlEventMap.erase(item);
  }
  for (auto item : closedPlists) {
    m_plistMap.erase(item);
  }
}

//---------------------------------------------------------
// generate XML:IDs
//---------------------------------------------------------

/**
 * Integer hash methods used for ID generation
 */

uint32_t MeiExporter::hash(uint32_t number, bool reverse) {
  const uint32_t magicNumber = reverse ? 0x119de1f3 : 0x45d9f3b;
  number = ((number >> 16) ^ number) * magicNumber;
  number = ((number >> 16) ^ number) * magicNumber;
  number = (number >> 16) ^ number;
  return number;
}

/**
 * Base encode a value into a std::string
 */

std::string MeiExporter::baseEncodeInt(uint32_t value, uint8_t base) {
  if ((base < 11) || (base > 62)) {
    return "";
  }

  static const std::string base62Chars =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

  std::string base62;
  if (value < base) {
    return std::string(1, base62Chars[value]);
  }

  while (value) {
    base62 += base62Chars[value % base];
    value /= base;
  }

  reverse(base62.begin(), base62.end());
  return base62;
}

/**
 * Generate an xml:id using the hash method and the m_xmlIdCounter.
 */

std::string MeiExporter::generateHashID() {
  uint32_t nr = hash(++m_xmlIDCounter, false);

  return this->baseEncodeInt(nr, 36);
}

/**
 * Return the @xml:id for an element.
 * First look in the UIDRegister if xml:id of the element has been registered
 * and can be preserved. Otherwise generate a new hash xml:id. Init the
 * m_xmlIdCounter when the method is called for the first time.
 */

std::string MeiExporter::getXmlIdFor(const EngravingItem *item, const char c) {
  if (!item) {
    return std::string();
  }

  auto cached = m_exportedXmlIds.find(item);
  if (cached != m_exportedXmlIds.end()) {
    return cached->second;
  }

  const bool useMuseScoreIds = configuration()->meiUseMuseScoreIds();
  std::string xmlId;

  if (useMuseScoreIds) {
    EID eid = item->eid();
    if (!eid.isValid()) {
      eid = item->assignNewEID();
    }
    String eidStr = String::fromStdString(eid.toStdString().c_str());
    xmlId =
        "mscore-" + eidStr.replace('/', '.').replace('+', '-').toStdString();
    if (m_uids) {
      m_uids->reg(item, xmlId);
    }
  } else if (m_uids && m_uids->hasUid(item)) {
    xmlId = m_uids->uid(item);
  } else {
    if (m_xmlIDCounter == 0) {
      std::random_device rd;
      std::mt19937 randomGenerator(rd());
      // Use xml:id metaTag's hash to initialize IDs
      String scoreXmlId = m_score->metaTag(u"xml:id");
      if (!scoreXmlId.isEmpty()) {
        m_xmlIDCounter = static_cast<int>(scoreXmlId.hash());
      } else {
        m_xmlIDCounter = randomGenerator();
      }
    }

    xmlId = c + this->generateHashID();
    if (m_uids) {
      m_uids->reg(item, xmlId);
    }
  }

  m_exportedXmlIds[item] = xmlId;
  return xmlId;
}

/**
 * Reset all the sub-counters for layer elements (e.g., accid, chord, note,
 * etc.).
 */

void MeiExporter::resetLayerIDs() {
  std::fill(m_layerCounterFor.begin(), m_layerCounterFor.end(), 0);
}

/**
 * Return the current @xml:id for a section.
 */

std::string MeiExporter::getSectionXmlId() {
  return String("s%1").arg(++m_sectionCounter).toStdString();
}

/**
 * Return the current @xml:id for a measure.
 * Reset the staff counter and the measure sub-counters
 */

std::string MeiExporter::getMeasureXmlId(const Measure *measure) {
  // Reset the staff counter when a new measure starts
  m_staffCounter = 0;
  m_measureCounter++;
  // Get the map IDs for the measure
  return this->getXmlIdFor(measure, 'm');
}

/**
 * Return the current @xml:id for a staff.
 * Reset the layer counters
 */

std::string MeiExporter::getStaffXmlId() {
  // Reset the layer counter when a new staff starts
  m_layerCounter = 0;
  return String("m%1s%2")
      .arg(m_measureCounter)
      .arg(++m_staffCounter)
      .toStdString();
}

/**
 * Return the current @xml:id for a layer.
 * Reset the layer sub-counters
 */

std::string MeiExporter::getLayerXmlId() {
  // Reset the layer sub-counters when a new layer starts
  this->resetLayerIDs();
  return String("m%1s%2l%3")
      .arg(m_measureCounter)
      .arg(m_staffCounter)
      .arg(++m_layerCounter)
      .toStdString();
}

/**
 * Return the current counter-based @xml:id for a layer element.
 */

std::string MeiExporter::getLayerXmlIdFor(layerElementCounter elementType) {
  String id;
  if (MEI_COUNTER_BASED_IDS) {
    // m (Measure) / s (Staff) / l (Layer) / ? Layer element type
    // The layer element abbreviation is given in the
    // MeiExporter::s_layerXmlIdMap
    id = String("m%1s%2l%3%4%5")
             .arg(m_measureCounter)
             .arg(m_staffCounter)
             .arg(m_layerCounter)
             .arg(MeiExporter::s_layerXmlIdMap.at(elementType))
             .arg(++(m_layerCounterFor.at(elementType)));
  }
  return id.toStdString();
}

/**
 * Return true if the used symbol is a laissez vibrer
 */

bool MeiExporter::isLaissezVibrer(const SymId id) {
  return id == SymId::articLaissezVibrerAbove ||
         id == SymId::articLaissezVibrerBelow;
}

static double calculateDirectiveYOffset(const EngravingItem *dir,
                                        const ChordRest *anchor) {
  if (!dir || !anchor) {
    return 0.0;
  }

  const Staff *staff = anchor->staff();
  if (!staff) {
    return 0.0;
  }

  const StaffType *staffType = staff->staffTypeForElement(anchor);
  const double spatium = staff->spatium(anchor->tick());
  const double lineDist = spatium * staffType->lineDistance().val();
  const int lines = staffType->lines();
  if (lineDist <= 0.0 || lines <= 0) {
    return 0.0;
  }

  // Unity applies positive offsets from the staff top edge and negative
  // offsets from the staff bottom edge. EngravingItem::y() includes both the
  // laid-out position and the manual offset.
  const double staffEdgeY =
      dir->placeBelow() ? ((lines - 1) * lineDist) : 0.0;
  return (staffEdgeY - dir->y()) / lineDist;
}

std::optional<std::pair<double, double>>
MeiExporter::getFingeringOffsets(const Fingering *fing,
                                 const std::string &startid) const {
  if (!fing) {
    return std::nullopt;
  }

  const std::string anchorXmlId =
      (!startid.empty() && startid.front() == '#') ? startid.substr(1)
                                                    : startid;

  const EngravingItem *anchor = nullptr;
  if (!anchorXmlId.empty()) {
    const auto exportedId = std::find_if(
        m_exportedXmlIds.cbegin(), m_exportedXmlIds.cend(),
        [&anchorXmlId](const auto &entry) { return entry.second == anchorXmlId; });
    if (exportedId != m_exportedXmlIds.cend()) {
      anchor = exportedId->first;
    }
  }

  if (!anchor) {
    anchor = fing->note();
  }

  const ChordRest *anchorChordRest = nullptr;
  PointF anchorPos;
  if (anchor && anchor->isNote()) {
    const Note *anchorNote = toNote(anchor);
    if (!anchorNote) {
      return std::nullopt;
    }

    anchorPos = anchorNote->pagePos();
    anchorChordRest = anchorNote->chord();
  } else if (anchor && anchor->isChord()) {
    const Chord *anchorChord = toChord(anchor);
    if (!anchorChord) {
      return std::nullopt;
    }

    anchorPos = anchorChord->pagePos();
    anchorChordRest = anchorChord;
  } else {
    return std::nullopt;
  }

  if (!anchorChordRest) {
    return std::nullopt;
  }

  const Staff *staff = anchorChordRest->staff();
  if (!staff) {
    return std::nullopt;
  }

  const StaffType *staffType = staff->staffTypeForElement(anchorChordRest);
  if (!staffType) {
    return std::nullopt;
  }

  const double spatium = staff->spatium(anchorChordRest->tick());
  const double lineDist = spatium * staffType->lineDistance().val();
  if (lineDist <= 0.0) {
    return std::nullopt;
  }

  // Use the same centered fingering position exported in pm:xy.
  const RectF fingeringRect = fing->pageBoundingRect();
  const double fingeringCenterX =
      fingeringRect.x() + (fingeringRect.width() / 2.0);
  const double fingeringCenterY =
      fingeringRect.y() + (fingeringRect.height() / 2.0);

  const double xOffset = (fingeringCenterX - anchorPos.x()) / lineDist;
  const double yOffset = -(fingeringCenterY - anchorPos.y()) / lineDist;
  return std::make_pair(xOffset, yOffset);
}

double MeiExporter::getOrnamentYOffset(const Ornament *ornament) const {
  if (!ornament) {
    return 0.0;
  }

  const ChordRest *anchor = ornament->chordRest();
  if (!anchor) {
    return 0.0;
  }

  const Staff *staff = anchor->staff();
  if (!staff) {
    return 0.0;
  }

  const StaffType *staffType = staff->staffTypeForElement(anchor);
  if (!staffType) {
    return 0.0;
  }

  const double spatium = staff->spatium(anchor->tick());
  const double lineDist = spatium * staffType->lineDistance().val();
  if (lineDist <= 0.0) {
    return 0.0;
  }

  double anchorY = anchor->pagePos().y();
  if (anchor->isChord()) {
    std::vector<const Note *> notes = visibleNotes(toChord(anchor));
    if (!notes.empty()) {
      anchorY = notes.front()->pagePos().y();
    }
  }

  RectF symbolBbox = ornament->symBbox(ornament->symId());
  symbolBbox = symbolBbox.translated(-0.5 * symbolBbox.width(), 0.0);

  const double ornamentCenterY =
      ornament->pagePos().y() + symbolBbox.y() + (symbolBbox.height() / 2.0);

  return -(ornamentCenterY - anchorY) / lineDist;
}

// Calculate y-position in staff spaces relative to the bottom edge of the
// bottom note of a MuseScore chord (which can be a single note).
double MeiExporter::getArticulationYOffset(const Articulation *articulation,
                                           const Chord *chord) {
  if (!articulation || !chord) {
    return 0.0;
  }

  const Staff *staff = chord->staff();
  const StaffType *staffType = staff->staffTypeForElement(chord);
  double spatium = staff->spatium(chord->tick());
  double lineDist = spatium * staffType->lineDistance().val();

  // Get the center of the bottom note's position.
  double bottomNoteCenterY = chord->downPos();

  // Get articulation's position relative to it's corresponding chord (note).
  double articY = articulation->y();

  // Negate the result so positive values indicate positions above the note.
  double distanceInStaffSpaces = -(articY - bottomNoteCenterY) / lineDist;

  // Return the precise value; formatting will be handled during export.
  return distanceInStaffSpaces;
}

double MeiExporter::getDynamicYOffset(const Dynamic *dynamic) {
  IF_ASSERT_FAILED(dynamic) { return 0.0; }

  const Staff *staff = dynamic->staff();

  double spatium = staff->spatium(dynamic->tick());

  // Get dynamic's position relative to staff.
  double dynamicY = dynamic->y();

  // Calculate offset in spatium units.
  // Positive values are above the staff, negative are below.
  double yOffset = -(dynamicY / spatium);

  return yOffset;
}

double MeiExporter::getDirectiveYOffset(const TextBase *dir) {
  if (!dir) {
    return 0.0;
  }

  const Segment *segment = toSegment(dir->explicitParent());
  if (!segment) {
    return 0.0;
  }

  const EngravingItem *element = segment->element(dir->track());
  if (!element || !element->isChordRest()) {
    return 0.0;
  }

  return calculateDirectiveYOffset(dir, toChordRest(element));
}

double MeiExporter::getDirectiveYOffset(const TextLineBase *dir) {
  if (!dir) {
    return 0.0;
  }

  const Segment *segment = dir->startSegment();
  if (!segment) {
    return 0.0;
  }

  const EngravingItem *element = segment->element(dir->track());
  if (!element || !element->isChordRest()) {
    return 0.0;
  }

  return calculateDirectiveYOffset(dir, toChordRest(element));
}

double MeiExporter::getRepeatMarkYOffset(const TextBase *repeatMark,
                                         const Measure *measure) const {
  if (!repeatMark || !measure) {
    return 0.0;
  }

  const Staff *staff = repeatMark->staff();
  if (!staff) {
    return 0.0;
  }

  const StaffType *staffType = staff->staffTypeForElement(repeatMark);
  if (!staffType) {
    return 0.0;
  }

  const double spatium = staff->spatium(repeatMark->tick());
  const double lineDist = spatium * staffType->lineDistance().val();
  const int lines = staffType->lines();
  if (lineDist <= 0.0 || lines <= 0) {
    return 0.0;
  }

  const RectF repeatMarkRect = repeatMark->pageBoundingRect();
  const double repeatMarkCenterY =
      repeatMarkRect.y() + (repeatMarkRect.height() / 2.0);
  const double staffOriginY = repeatMark->pagePos().y() - repeatMark->pos().y();
  const double staffBottomY = staffOriginY + ((lines - 1) * lineDist);

  return (staffBottomY - repeatMarkCenterY) / lineDist;
}

double MeiExporter::getFermataYOffset(const Fermata *fermata) {
  if (!fermata) {
    return 0.0;
  }

  // Get the chord/rest from the segment using the fermata's track
  const Segment *segment = fermata->segment();
  if (!segment) {
    return 0.0;
  }

  const EngravingItem *element = segment->element(fermata->track());
  if (!element || !element->isChordRest()) {
    return 0.0;
  }

  const ChordRest *cr = toChordRest(element);
  const Staff *staff = cr->staff();
  const StaffType *staffType = staff->staffTypeForElement(cr);
  double spatium = staff->spatium(cr->tick());
  double lineDist = spatium * staffType->lineDistance().val();

  // Get the fermata's final rendered position (after autoplace and offset)
  // This includes the base position + offset + autoplace adjustments
  double fermataY = fermata->y() + fermata->offset().y();

  // Calculate the anchor point based on the chord/rest type
  double anchorY;
  if (cr->isChord()) {
    const Chord *chord = toChord(cr);
    // For chords, use the bottom note position as anchor
    anchorY = chord->downPos();
  } else if (cr->isRest()) {
    const Rest *rest = toRest(cr);
    // For rests, use the rest's position relative to staff
    anchorY = rest->pos().y();
  } else {
    // Fallback to chord/rest position
    anchorY = cr->y();
  }

  // Calculate offset in staff spaces
  // Positive values indicate fermata is above the anchor
  double yOffset = -(fermataY - anchorY) / lineDist;

  return yOffset;
}

// Calculate y-position in staff spaces relative to bottom note.
// Returns a "yOffset" double to the hairpin equal to number of staff spaces
// between the hairpin and the bottom note of the note/chord. Calculate
// y-position in staff spaces relative to bottom note. Returns a "yOffset"
// double to the hairpin equal to number of staff spaces between the hairpin and
// the bottom note of the note/chord.
double MeiExporter::getHairpinYOffset(const Hairpin *hairpin) {
  if (!hairpin) {
    return 0.0;
  }

  const Staff *staff = hairpin->staff();
  double spatium = staff->spatium(hairpin->tick());

  // Get the first segment of the hairpin spanner, which contains the actual
  // rendered position after layout and collision detection (same approach as
  // ottava)
  const SpannerSegment *segment = hairpin->spannerSegments().front();

  // Get hairpin's Y position in PAGE coordinates (same as anchor lines use)
  double hairpinY = segment->pagePos().y();

  // Get the system through the hairpin's start measure using the safe method
  const Measure *startMeasure = hairpin->findStartMeasure();
  const System *system = startMeasure->system();

  // Use EXACTLY the same logic as the anchor lines
  const staff_idx_t stIdx = staff->idx();
  double staffY =
      system->staffYpage(stIdx); // Top of the individual staff (page-relative)

  // Adjust for staff placement (above/below) - EXACT same logic as anchor lines
  if (hairpin->placement() == PlacementV::BELOW) {
    // For below placement, add the staff height to get the bottom edge
    staffY += system->staff(stIdx)->bbox().height();
  }

  // Add staff type offset - EXACT same as anchor lines
  staffY += hairpin->staffOffsetY();

  // Calculate distance in spatiums
  // Positive values mean hairpin is above the staff anchor
  double distanceInSpatiums = (staffY - hairpinY) / spatium;

  return distanceInSpatiums;
}

double MeiExporter::getOctaveYOffset(const Ottava *ottava) {
  if (!ottava) {
    return 0.0;
  }

  const Staff *staff = ottava->staff();
  double spatium = staff->spatium(ottava->tick());

  // Get the first segment of the ottava spanner, which contains the actual
  // rendered position after layout and collision detection
  const SpannerSegment *segment = ottava->spannerSegments().front();
  if (!segment) {
    return 0.0;
  }

  // Get octave's position relative to staff from the segment
  double octaveY = segment->pos().y();

  // Calculate offset in spatium units.
  // Positive values are above the staff, negative are below.
  double yOffset = -(octaveY / spatium);

  // If octave is placed above, add staff height (4 spatium).
  if (ottava->placement() == PlacementV::ABOVE) {
    yOffset += 4.0;
  }

  return yOffset;
}

double MeiExporter::getOctaveEndHookHeight(const Ottava *ottava) {
  if (!ottava) {
    return 0.0;
  }

  return ottava->getProperty(Pid::END_HOOK_HEIGHT).value<Spatium>().val();
}

double MeiExporter::getVoltaLineYOffset(const VoltaSegment *segment) const {
  if (!segment) {
    return 0.0;
  }

  const Volta *volta = segment->volta();
  const Staff *staff = volta ? volta->staff() : nullptr;
  const System *system = segment->system();
  const staff_idx_t staffIdx = segment->staffIdx();

  if (!volta || !staff || !system || staffIdx == muse::nidx) {
    return 0.0;
  }

  const double spatium = staff->spatium(volta->tick());
  const double staffTopY = system->staffYpage(staffIdx);
  const double anchorY = staffTopY + volta->staffOffsetY();

  // Convert the distance between the staff's top line and the volta line
  // into spatium units so it matches ottava serialization.
  const double offset = anchorY - segment->pagePos().y();
  return offset / spatium;
}

double MeiExporter::getVoltaLabelYOffset(const VoltaSegment *segment) const {
  if (!segment || !segment->text()) {
    return 0.0;
  }

  const Volta *volta = segment->volta();
  const Staff *staff = volta ? volta->staff() : nullptr;
  const System *system = segment->system();
  const staff_idx_t staffIdx = segment->staffIdx();

  if (!volta || !staff || !system || staffIdx == muse::nidx) {
    return 0.0;
  }

  const double spatium = staff->spatium(volta->tick());
  const double staffTopY = system->staffYpage(staffIdx);
  const double anchorY = staffTopY + volta->staffOffsetY();

  const double offset = anchorY - segment->text()->pagePos().y();
  return offset / spatium;
}

double MeiExporter::getRestYOffset(const Rest *rest) {
  if (!rest) {
    return 0.0;
  }

  const Staff *staff = rest->staff();
  const StaffType *staffType = staff->staffTypeForElement(rest);
  double spatium = staff->spatium(rest->tick());
  double lineDist = spatium * staffType->lineDistance().val();
  int lines = staffType->lines();

  // Get rest's Y position relative to staff
  double restY = rest->pos().y();

  // Get staff bottom line Y position
  double staffBottomY = (lines - 1) * lineDist;

  // Calculate distance from bottom line in staff spaces
  // Positive values indicate positions above the bottom line
  double distanceFromBottom = (staffBottomY - restY) / lineDist;

  return distanceFromBottom;
}

double MeiExporter::getStemLength(const Chord *chord) {
  if (!chord || chord->durationType().type() == DurationType::V_WHOLE) {
    return 0.0;
  }

  const Staff *staff = chord->staff();
  double spatium = staff->spatium(chord->tick());

  // Get the actual stem length in staff spaces.
  const Stem *stem = chord->stem();
  if (!stem) {
    return 0.0;
  }
  double stemLength = stem->length() / spatium;

  return stemLength;
}

double
MeiExporter::calculateBeatPosition(const engraving::ChordRest *cr) const {
  if (!cr) {
    return 0.0;
  }

  const Measure *measure = cr->measure();
  if (!measure) {
    return 0.0;
  }

  // Get the time signature for the measure.
  TimeSigFrac timeSig = measure->timesig();

  // Use the dUnitTicks() method to get the number of ticks for the
  // beat unit defined by the time signature's denominator (e.g., an eighth note
  // in 6/8). This is a more direct and reliable method than calculating it
  // manually.
  const double ticksPerBeatUnit = timeSig.dUnitTicks();

  if (ticksPerBeatUnit == 0) { // Avoid division by zero.
    return 0.0;
  }

  // Get the element's position within the measure in ticks.
  // The tick() value for elements within a tuplet is already scaled by
  // MuseScore's internal logic, so no special handling for tuplets is required
  // here.
  Fraction posInMeasure = cr->tick() - measure->tick();

  // Calculate the beat position based on the number of denominator units.
  // This calculation now naturally handles both regular and tuplet-based notes
  // because their tick values are pre-adjusted. Beats are 1-indexed.
  double beatPosition = (posInMeasure.ticks() / ticksPerBeatUnit) + 1.0;

  return beatPosition;
}

double
MeiExporter::calculateGraceNoteBeatPosition(const engraving::Chord *graceChord,
                                            const engraving::Chord *parentChord,
                                            bool isAfter) const {
  if (!graceChord || !parentChord) {
    return 0.0;
  }

  const Measure *measure = parentChord->measure();
  if (!measure) {
    return 0.0;
  }

  // Get the time signature for the measure.
  TimeSigFrac timeSig = measure->timesig();
  const double ticksPerBeatUnit = timeSig.dUnitTicks();

  if (ticksPerBeatUnit == 0) {
    return 0.0;
  }

  // Get the parent note's position within the measure
  Fraction parentPosInMeasure = parentChord->tick() - measure->tick();
  double parentBeatPosition =
      (parentPosInMeasure.ticks() / ticksPerBeatUnit) + 1.0;

  // Determine grace note type
  bool isAcciaccatura =
      (graceChord->noteType() == engraving::NoteType::ACCIACCATURA);

  // Calculate grace note duration in ticks
  Fraction graceDuration = graceChord->ticks();

  if (isAcciaccatura) {
    // For acciaccaturas, treat them as half the duration of the parent note
    Fraction parentDuration = parentChord->ticks();
    graceDuration = parentDuration / 2;
  }

  // Convert grace duration to beat units
  double graceDurationInBeats = graceDuration.ticks() / ticksPerBeatUnit;

  if (isAfter) {
    // Grace notes after the parent note
    return parentBeatPosition + graceDurationInBeats;
  } else {
    // Grace notes before the parent note
    double graceBeatPosition = parentBeatPosition - graceDurationInBeats;

    // If the grace note would be before beat 1, use fractional beat values
    if (graceBeatPosition < 1.0) {
      // For grace notes at the beginning of a measure, use values like 0.5,
      // 0.75, etc. Calculate how much before beat 1 they should be double
      // beforeBeat1 = 1.0 - graceBeatPosition; // TODO: Use this for proper
      // grace note positioning

      // Distribute grace notes evenly before beat 1
      // For multiple grace notes, we'd need to know their order
      // For now, use a simple approach: first grace note gets 0.5, second gets
      // 0.75, etc. This is a simplification - in practice, we'd need to track
      // the grace note order

      // Check if this is the first grace note in the group
      const Chord *parent = parentChord;
      if (parent->graceNotesBefore().size() > 0) {
        // Find the position of this grace chord in the grace notes group
        size_t graceIndex = 0;
        for (const Chord *gc : parent->graceNotesBefore()) {
          if (gc == graceChord) {
            break;
          }
          graceIndex++;
        }

        // Calculate beat position: 0.5 for first, 0.75 for second, etc.
        graceBeatPosition = 0.5 + (graceIndex * 0.25);
      } else {
        graceBeatPosition = 0.5; // Default for single grace note
      }
    }

    return graceBeatPosition;
  }
}

std::string MeiExporter::formatDecimalStr(double value, int precision) const {
  std::stringstream ss;
  ss << std::fixed << std::setprecision(precision) << value;
  return ss.str();
}

std::string MeiExporter::formatBeatPosition(double beatPosition,
                                            int precision) const {
  return formatDecimalStr(beatPosition, precision);
}

bool MeiExporter::writeGraceNote(const Note *note, const Chord *graceChord,
                                 const Chord *parentChord, const Staff *staff,
                                 bool isChord, bool isAfter) {
  if (!shouldExportNote(note)) {
    return true;
  }

  IF_ASSERT_FAILED(note && graceChord && parentChord && staff) { return false; }

  Interval interval = staff->part()->instrument()->transpose();
  auto [meiNote, meiAccid] =
      Convert::pitchToMEI(note, note->accidental(), interval);
  m_currentNode = m_currentNode.append_child();
  if (!isChord) {
    meiNote.SetDur(Convert::durToMEI(graceChord->durationType().type()));
    if (graceChord->dots()) {
      meiNote.SetDots(graceChord->dots());
    }
    this->writeBeamTypeAtt(graceChord, meiNote);
    this->writeStaffIdentAtt(graceChord, staff, meiNote);
    this->writeStemAtt(graceChord, meiNote);
    this->writeArtics(graceChord);
    this->writeVerses(graceChord);

    // Add stem length attribute if the note has a stem
    if (graceChord &&
        graceChord->durationType().type() != DurationType::V_WHOLE) {
      double stemLength = getStemLength(graceChord);
      if (stemLength > 0.0) {
        meiNote.SetStemLen(stemLength);
      }
    }
  } else {
    // Add stem direction for notes within chords.
    this->writeStemAtt(graceChord, meiNote);
  }

  Convert::colorToMEI(note, meiNote);
  std::string xmlId = this->getXmlIdFor(note, 'n');
  meiNote.Write(m_currentNode, xmlId);
  m_noteXmlIdCache[note] = xmlId;

  // Add beat attribute for grace note using special calculation.
  double beatPosition =
      calculateGraceNoteBeatPosition(graceChord, parentChord, isAfter);
  const std::string beat = formatBeatPosition(beatPosition);
  m_currentNode.append_attribute("beat") = beat.c_str();

  // Pianomania: export grace notehead anchor position in inches.
  PointF notePos = note->pagePos();
  double nx = notePos.x() / DPI;
  double ny = toBottomLeftInches(notePos.y());
  std::string noteXY = formatDecimalStr(nx, 3) + std::string(",") +
                       formatDecimalStr(ny, 3);
  m_currentNode.append_attribute("pm:xy") = noteXY.c_str();

  // Use precomputed index assigned in MeiExporter::assignIndicesForMeasure.
  auto it =
      m_noteIdxAssignment.find(note); // Find the note in the precomputed index.
  if (it == m_noteIdxAssignment.end()) {
    LOGE() << "MEI export is missing a precomputed grace-note index";
    return false;
  }
  m_currentNode.append_attribute("idx") =
      it->second; // Append the index to the current node.

  m_pianomaniaNoteRecords.push_back(
      {note, xmlId, this->getXmlIdFor(graceChord, 'c'),
       this->getMeasureXmlId(parentChord->measure()), beat, it->second});

  // Add held attribute if the note has the PIANOMANIA_HELD_NOTE property set to
  // true
  if (note->getProperty(mu::engraving::Pid::PIANOMANIA_HELD_NOTE).toBool()) {
    m_currentNode.append_attribute("held") = "true";
  }
  appendPianomaniaHeldPulseAttributes(m_currentNode, note);
  if (!appendPianomaniaHeldPitchCurveAttribute(m_currentNode, note)) {
    return false;
  }
  // Add shake attribute if the note has the PIANOMANIA_SHAKE_NOTE property set
  // to true
  if (note->getProperty(mu::engraving::Pid::PIANOMANIA_SHAKE_NOTE).toBool()) {
    m_currentNode.append_attribute("shake") = "true";
  }
  int handValue = note->getProperty(mu::engraving::Pid::PIANOMANIA_HAND).toInt();
  if (handValue == static_cast<int>(mu::engraving::Note::PianomaniaHand::Left)) {
    m_currentNode.append_attribute("hand") = "left";
  } else if (handValue ==
             static_cast<int>(mu::engraving::Note::PianomaniaHand::Right)) {
    m_currentNode.append_attribute("hand") = "right";
  }

  if (!isChord) {
    this->fillControlEventMap(xmlId, graceChord, false);
  }

  if (note->tieFor()) {
    m_startingControlEventList.push_back(
        std::make_pair(note->tieFor(), "#" + xmlId));
  }
  if (note->tieBack()) {
    m_endingControlEventMap[note->tieBack()] = "#" + xmlId;
  }

  for (const EngravingItem *element : note->el()) {
    if (element->isFingering()) {
      m_startingControlEventList.push_back(
          std::make_pair(element, "#" + xmlId));
    }
  }

  if (meiAccid.HasAccid() || meiAccid.HasAccidGes()) {
    pugi::xml_node accidNode = m_currentNode.append_child();
    Accidental *acc = note->accidental();
    PointF accPos;
    bool hasAccPos = false;
    if (acc) {
      Convert::colorToMEI(acc, meiAccid);
      std::string xmlIdAcc = this->getXmlIdFor(acc, 'a');
      meiAccid.Write(accidNode, xmlIdAcc);
      accPos = acc->pagePos();
      hasAccPos = true;
    } else {
      meiAccid.Write(accidNode, this->getLayerXmlIdFor(ACCID_L));
      PointF graceNotePos = note->pagePos();
      double accidentalDistanceMm =
          note->style().styleMM(Sid::accidentalDistance);
      static constexpr double MM_PER_INCH = 25.4;
      double fallbackOffset = (accidentalDistanceMm / MM_PER_INCH) * DPI;
      graceNotePos.setX(graceNotePos.x() - fallbackOffset);
      accPos = graceNotePos;
      hasAccPos = true;
    }

    if (hasAccPos && meiAccid.HasAccid()) {
      double ax = accPos.x() / DPI;
      double ay = toBottomLeftInches(accPos.y());
      std::string accXY = formatDecimalStr(ax, 3) + std::string(",") +
                          formatDecimalStr(ay, 3);
      accidNode.append_attribute("pm:xy") = accXY.c_str();
    }
  }

  // non critical assert
  assert(isCurrentNode(meiNote));
  m_currentNode = m_currentNode.parent();

  return true;
}

//---------------------------------------------------------
// Full Score Rendering Methods
//---------------------------------------------------------

/**
 * Adds a <facsimile><surface .../></facsimile> describing the page/canvas size
 * in inches. Prefers Paint::pageSizeInch(); falls back to style if needed.
 * Note: Ensure score is set to Continuous (vertical) layout for this to work
 * properly.
 */
void MeiExporter::writeSurfaceSizeInches(pugi::xml_node &meiHead) {
  // Get page size in inches.
  double widthIn = 0.0;
  double heightIn = 0.0;

  if (m_score) {
    const SizeF pageSizeIn =
        mu::engraving::rendering::score::Paint::pageSizeInch(m_score);
    if (pageSizeIn.width() > 0.0 && pageSizeIn.height() > 0.0) {
      widthIn = pageSizeIn.width();
      heightIn = pageSizeIn.height();
    } else {
      // Fallback to style-defined size (also in inches).
      widthIn = m_score->style().styleD(Sid::pageWidth);
      heightIn = m_score->style().styleD(Sid::pageHeight);
    }
  }

  // Emit facsimile/surface with explicit corners and units.
  pugi::xml_node facsimile = meiHead.append_child("facsimile");

  pugi::xml_node surface = facsimile.append_child("surface");
  surface.append_attribute("xml:id") = "pg1";
  // Format with three decimals for stable text diffs.
  auto fmt3 = [](double v) { return QString::number(v, 'f', 3); };
  surface.append_attribute("upperLeftX") = "0";
  surface.append_attribute("upperLeftY") = fmt3(heightIn).toStdString().c_str();
  surface.append_attribute("lowerRightX") = fmt3(widthIn).toStdString().c_str();
  surface.append_attribute("lowerRightY") = "0";

  surface.append_attribute("pm:units") = "in";
  // Rendering-resolution contract for MEI consumers (Unity sizes its practice
  // score captures from this), not an engraving internal. Geometry is exported
  // in inches, so this is deliberately pinned instead of tracking MuseScore's
  // DPI constant, which changed from 360 to 1200 in 4.7.
  surface.append_attribute("pm:dpi") = std::to_string(PIANOMANIA_MEI_DPI).c_str();
  appendSpatiumInches(surface);

  // Later, when writing <body>, add: <pb n="1" facs="#pg1"/>.
}

/**
 * Appends the MuseScore spatium size in inches as pm:spatium on the surface
 * element.
 */
void MeiExporter::appendSpatiumInches(pugi::xml_node &surface) {
  double spatiumInches = 0.0;
  if (m_score) {
    // style().spatium() is stored in millimetres * DPMM. Convert to inches.
    spatiumInches = m_score->style().spatium() / (INCH * DPMM);
  }

  auto fmt6 = [](double v) { return QString::number(v, 'f', 6); };
  surface.append_attribute("pm:spatium") =
      fmt6(spatiumInches).toStdString().c_str();
}
#include "engraving/dom/spanner.h"
