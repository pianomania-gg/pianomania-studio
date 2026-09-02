# SPDX-License-Identifier: GPL-3.0-only
#
# Headless WebAssembly profile for the in-browser Pianomania converter.
#
# Used as a CMake initial-cache file:
#   emcmake cmake -C buildscripts/wasm/wasm-headless.cmake ...
#
# Goal: link ONLY the conversion path (global/draw/engraving/importexport[mei,midi]/
# project/converter + the pmwasm Embind entry point) and drop every GUI/audio/QML
# module. Smaller binary, no Qt Widgets/QML dependency, no audio backend.

# --- GUI / interactive modules: OFF ---
set(MUE_BUILD_APPSHELL_MODULE        OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_NOTATION_MODULE        OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_PALETTE_MODULE         OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_INSPECTOR_MODULE       OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_INSTRUMENTSSCENE_MODULE OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_PLAYBACK_MODULE        OFF CACHE BOOL "" FORCE)
# The preferences QML module declares a dependency on muse_ui_qml, which does not
# exist once the UI module is off, so configuration stops there. A converter has
# no preferences dialog to show.
set(MUE_BUILD_PREFERENCES_MODULE     OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_MUSESOUNDS_MODULE      OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_BRAILLE_MODULE         OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_VIDEOEXPORT_MODULE     OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_IMAGESEXPORT_MODULE    OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_ENGRAVING_PLAYBACK     OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_ENGRAVING_DEVTOOLS     OFF CACHE BOOL "" FORCE)

# --- Conversion path: ON ---
# importexport (mei+midi) is the only conversion module built for wasm. The
# converter + project modules are editor-coupled (project pulls QML view-models +
# notation + uicomponents), so the headless converter bypasses them: pmwasm loads
# the score via engraving's EngravingProject/MscReader directly, exports MEI via
# MeiExporter, and compiles pianomaniaexport.cpp (engraving-only) for MIDI.
set(MUE_BUILD_IMPORTEXPORT_MODULE    ON  CACHE BOOL "" FORCE)
set(MUE_BUILD_CONVERTER_MODULE       OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_PROJECT_MODULE         OFF CACHE BOOL "" FORCE)

# --- Tests / extras: OFF (don't build for wasm) ---
set(MUSE_ENABLE_UNIT_TESTS           OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_ENGRAVING_TESTS        OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_IMPORTEXPORT_TESTS     OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_NOTATION_TESTS         OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_PLAYBACK_TESTS         OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_PROJECT_TESTS          OFF CACHE BOOL "" FORCE)
set(MUE_BUILD_BRAILLE_TESTS          OFF CACHE BOOL "" FORCE)

# --- Soundfont: not needed for notation->MEI/MIDI conversion ---
set(MUE_DOWNLOAD_SOUNDFONT           OFF CACHE BOOL "" FORCE)
set(MUE_INSTALL_SOUNDFONT            OFF CACHE BOOL "" FORCE)
set(MUE_RUN_LRELEASE                 OFF CACHE BOOL "" FORCE)

# --- muse framework modules: keep only what the conversion path needs ---
# Disabled modules fall back to their stubs in appfactory (same class names), so
# the app still links. Kept: actions, draw, midi, mpe (engraving types), global.
# accessibility + network kept ON (real): newWasmApp uses accessibility, project
# links network, and their stubs are stale. Both are lightweight + in the wasm kit.
set(MUSE_MODULE_AUDIO          OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_AUDIOPLUGINS   OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_CLOUD          OFF CACHE BOOL "" FORCE)
# Docking windows are desktop chrome, and the module needs Qt's private Gui
# headers, which the WebAssembly kit does not ship. It was building only because
# a macOS kit happens to carry them. APP-WEB switches it off for the same reason.
set(MUSE_MODULE_DOCKWINDOW     OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_LANGUAGES      OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_LEARN          OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_MULTIINSTANCES OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_MUSESAMPLER    OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_SHORTCUTS      OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_TOURS          OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_UI             OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_UPDATE         OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_VST            OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_WORKSPACE      OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_EXTENSIONS     OFF CACHE BOOL "" FORCE)
set(MUSE_MODULE_AUTOBOT        OFF CACHE BOOL "" FORCE)

# Use FreeType font metrics, NOT Qt's. QFontDatabase/QFontMetrics need a
# QGuiApplication; the headless converter runs QCoreApplication, so Qt font APIs
# trap. The FreeType fonts engine provides all metrics we need for MEI layout.
set(MUSE_MODULE_DRAW_USE_QTFONTMETRICS OFF CACHE BOOL "" FORCE)

set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
