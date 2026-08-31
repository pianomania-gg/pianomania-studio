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
#include "pmconverter.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h> // rmdir

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "engraving/engravingproject.h"
#include "engraving/infrastructure/mscreader.h"
#include "engraving/infrastructure/ifileinfoprovider.h"
#include "engraving/dom/factory.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"
#include "engraving/pm/pmlayout.h"
#include "engraving/pm/pmstyle.h"

#include "global/types/datetime.h"

#include "project/internal/pianomaniaexport.h"

#include "io/path.h"
#include "types/ret.h"
#include "log.h"

using namespace mu::pmwasm;

// ─────────────────────────────────────────────────────────────────────────────
// Module setup — captures the IoC context for EngravingProject::create().
// ─────────────────────────────────────────────────────────────────────────────

namespace {
muse::modularity::ContextPtr s_iocCtx;

// In-memory (MEMFS) working layout. Nothing is persisted; wiped each call. The
// user's score never leaves the browser tab.
constexpr const char* WORK_DIR = "/work";
constexpr const char* OUT_DIR = "/work/out";
constexpr const char* IN_PATH = "/work/in.mscz";
constexpr const char* OUT_BASE = "/work/out/song";   // -> song.mid, song-*.mid
constexpr const char* OUT_MEI = "/work/out/song.mei";
constexpr const char* MANIFEST = "/work/out/manifest.json";
}

PmWasmModule::PmWasmModule(const muse::modularity::ContextPtr& iocCtx)
    : m_iocCtx(iocCtx)
{
    // Capture the IoC context immediately. onStartApp() is deferred to the Qt event
    // loop (ConsoleApp::perform), which the headless wasm app never enters, so we
    // can't rely on it. The engraving/mei services are registered into this same
    // context during the synchronous onInit phase, before pmConvert() is ever called.
    s_iocCtx = iocCtx;
}

std::string PmWasmModule::moduleName() const
{
    return "pmwasm";
}

void PmWasmModule::onStartApp()
{
}

const muse::modularity::ContextPtr& mu::pmwasm::pmIocContext()
{
    return s_iocCtx;
}

// ─────────────────────────────────────────────────────────────────────────────
// MEMFS helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {
void rmTree(const std::string& path)
{
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        ::remove(path.c_str());
        return;
    }
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        rmTree(path + "/" + name);
    }
    closedir(dir);
    ::rmdir(path.c_str());
}

void resetWorkspace()
{
    rmTree(WORK_DIR);
    ::mkdir(WORK_DIR, 0777);
    ::mkdir(OUT_DIR, 0777);
}

bool writeBytes(const std::string& path, const char* data, size_t size)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(data, static_cast<std::streamsize>(size));
    return out.good();
}

bool readBytes(const std::string& path, std::string& outBytes)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    outBytes.resize(static_cast<size_t>(size));
    return static_cast<bool>(in.read(outBytes.data(), size));
}

emscripten::val makeFileEntry(const std::string& name, const std::string& bytes)
{
    emscripten::val u8 = emscripten::val::global("Uint8Array").new_(bytes.size());
    u8.call<void>("set", emscripten::val(emscripten::typed_memory_view(bytes.size(),
                                                                       reinterpret_cast<const unsigned char*>(bytes.data()))));
    emscripten::val entry = emscripten::val::object();
    entry.set("name", name);
    entry.set("bytes", u8);
    return entry;
}

void collectOutputs(emscripten::val& files)
{
    DIR* dir = opendir(OUT_DIR);
    if (!dir) {
        return;
    }
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        std::string full = std::string(OUT_DIR) + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        std::string bytes;
        if (readBytes(full, bytes)) {
            files.call<void>("push", makeFileEntry(name, bytes));
        }
    }
    closedir(dir);
}

emscripten::val errorResult(const std::string& message)
{
    emscripten::val res = emscripten::val::object();
    res.set("ok", false);
    res.set("error", message);
    res.set("files", emscripten::val::array());
    return res;
}

// Minimal file-info provider for the in-memory input. EngravingProject/MscLoader
// query masterScore->fileInfo() (e.g. for the doc name); the real impl lives in
// the editor-only project module, so provide a tiny one here.
class PmFileInfoProvider : public mu::engraving::IFileInfoProvider
{
public:
    muse::io::path_t path() const override { return muse::io::path_t(std::string(IN_PATH)); }
    muse::io::path_t fileName(bool includingExtension = true) const override
    {
        return muse::io::path_t(std::string(includingExtension ? "in.mscz" : "in"));
    }
    muse::io::path_t absoluteDirPath() const override { return muse::io::path_t(std::string(WORK_DIR)); }
    muse::String displayName() const override { return muse::String(u"in"); }
    muse::DateTime birthTime() const override { return muse::DateTime(); }
    muse::DateTime lastModified() const override { return muse::DateTime(); }
};

}

// ─────────────────────────────────────────────────────────────────────────────
// Embind entry point — pmConvert(Uint8Array) -> { ok, error, files[] }
// ─────────────────────────────────────────────────────────────────────────────

static emscripten::val pmConvert(const emscripten::val& input)
{
    if (!pmIocContext()) {
        return errorResult("Converter engine is not initialized.");
    }

    // Copy the incoming Uint8Array into a std::string buffer.
    const size_t length = input["length"].as<size_t>();
    std::string bytes;
    bytes.resize(length);
    emscripten::val memView = emscripten::val(emscripten::typed_memory_view(
                                                  length, reinterpret_cast<unsigned char*>(bytes.data())));
    memView.call<void>("set", input);

    resetWorkspace();
    if (!writeBytes(IN_PATH, bytes.data(), bytes.size())) {
        return errorResult("Failed to stage the input file in memory.");
    }

    // 1) Load the .mscz into a MasterScore via engraving (no project/notation).
    auto engravingProject = mu::engraving::EngravingProject::create(pmIocContext());
    if (!engravingProject) {
        return errorResult("Failed to create engraving project.");
    }
    // MscLoader needs a non-null fileInfo provider (doc name etc.).
    engravingProject->setFileInfoProvider(std::make_shared<PmFileInfoProvider>());

    mu::engraving::MscReader::Params params;
    params.filePath = muse::io::path_t(IN_PATH);
    params.mode = mu::engraving::MscIoMode::Zip;
    mu::engraving::MscReader reader(params);
    if (!reader.open()) {
        return errorResult("Could not open the file (not a valid MuseScore .mscz?).");
    }

    mu::engraving::SettingsCompat settingsCompat;
    muse::Ret ret = engravingProject->loadMscz(reader, settingsCompat, /*ignoreVersionError*/ false);
    reader.close();
    if (!ret) {
        return errorResult("Failed to read score: " + ret.toString());
    }

    ret = engravingProject->setupMasterScore(/*forceMode*/ false);
    if (!ret) {
        return errorResult("Failed to set up score: " + ret.toString());
    }

    mu::engraving::MasterScore* masterScore = engravingProject->masterScore();
    if (!masterScore) {
        return errorResult("No score found in the file.");
    }

    // 2) Normalize the page + part layout so Composer output matches Pianomania
    //    catalog geometry in-game. Source-file page settings otherwise leak
    //    through as portrait pages, tight margins, headers, and oversized staves.
    {
        using namespace mu::engraving;

        pm::stripHeaderFramesAndFooters(masterScore);
        pm::applyPianomaniaStyle(masterScore);

        // Single-staff sources (lead sheets, melodies) get an empty bass staff +
        // brace so the export is a grand staff like every catalog song. The game
        // renders grand staves only (MeiParserLib.GetInitialClefs defaults a
        // missing bottom staff to a bass clef drawn over the single system).
        if (masterScore->nstaves() == 1) {
            Staff* top = masterScore->staff(0);
            Part* part = top->part();
            KeyList keys = masterScore->keyList();

            Staff* bass = Factory::createStaff(part);
            bass->setDefaultClefType(ClefTypeList(ClefType::F));
            masterScore->undoInsertStaff(bass, 1, /*createRests*/ true);
            masterScore->adjustKeySigs(1, 2, keys);

            top->setBracketType(0, BracketType::BRACE);
            top->setBracketSpan(0, 2);
            top->setBarLineSpan(1); // barlines run through both staves
        } else if (masterScore->nstaves() == 2) {
            // Bracket-less two-staff sources still get the catalog brace +
            // through barlines so the export matches catalog piano scores.
            Staff* top = masterScore->staff(0);
            if (top->bracketType(0) == BracketType::NO_BRACKET) {
                top->setBracketType(0, BracketType::BRACE);
                top->setBracketSpan(0, 2);
                top->setBarLineSpan(1);
            }
        }
    }

    // 3) Lay out the score so MEI coordinates (pm:xy, beziers, …) are correct.
    masterScore->setUpTempoMap();
    masterScore->setPlaylistDirty();
    mu::engraving::pm::applyPianomaniaAutoLayout(masterScore);

    // 4) Coordinated MEI, MIDI variants, and provenance manifest.
    {
        muse::RetVal<mu::project::pianomania::PianomaniaExportResult> exportRet
            = mu::project::pianomania::exportPianomaniaBundle(masterScore,
                                                              muse::io::path_t(IN_PATH),
                                                              muse::io::path_t(OUT_BASE),
                                                              muse::io::path_t(OUT_MEI),
                                                              /*exportRpns*/ false);
        if (!exportRet.ret) {
            return errorResult("Pianomania export failed: " + exportRet.ret.toString());
        }

        if (!mu::project::pianomania::writePianomaniaManifest(muse::io::path_t(MANIFEST), exportRet.val)) {
            return errorResult("Failed to write Pianomania manifest.");
        }
    }

    emscripten::val files = emscripten::val::array();
    collectOutputs(files);
    if (files["length"].as<size_t>() == 0) {
        return errorResult("Conversion produced no output files.");
    }

    emscripten::val res = emscripten::val::object();
    res.set("ok", true);
    res.set("error", std::string());
    res.set("files", files);
    return res;
}

EMSCRIPTEN_BINDINGS(pm_converter)
{
    emscripten::function("pmConvert", &pmConvert);
}
