# SPDX-License-Identifier: GPL-3.0-only
# MuseScore-Studio-CLA-applies
#
# MuseScore Studio
# Music Composition & Notation
#
# Copyright (C) 2021-2024 MuseScore Limited and others
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

###########################################
# Setup main application
###########################################

set(EXECUTABLE_NAME mscore)

###########################################
# Platform specific
###########################################
include(GetPlatformInfo)
if (OS_IS_WIN)
    set(EXECUTABLE_NAME ${MUSE_APP_NAME}${MUSE_APP_VERSION_MAJOR})

    include(GetCompilerInfo)

    if (CC_IS_MSVC)
        # MSVC recognizes a *.rc file and will invoke the resource compiler to link it
        set(WINDOWS_ICONS_RC ${PROJECT_SOURCE_DIR}/share/icons/windows_icons.rc)
    elseif(CC_IS_MINGW)
        set(WINDOWS_ICONS_RC ${PROJECT_BINARY_DIR}/windows_icons_rc.o)
        add_custom_command(
            OUTPUT ${PROJECT_BINARY_DIR}/windows_icons_rc.o
            COMMAND ${CMAKE_RC_COMPILER} -i windows_icons.rc -o ${PROJECT_BINARY_DIR}/windows_icons_rc.o
            DEPENDS ${PROJECT_SOURCE_DIR}/share/icons/windows_icons.rc
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/share/icons
            )
        set_source_files_properties(${PROJECT_BINARY_DIR}/windows_icons_rc.o PROPERTIES generated true)
    endif()

elseif(OS_IS_LIN)

    # add ssl support on local linux machine
    # set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L /usr/local/lib -lcrypto -lssl")

    if (MUSE_APP_INSTALL_SUFFIX)
        set(EXECUTABLE_NAME "${EXECUTABLE_NAME}${MUSE_APP_INSTALL_SUFFIX}")
    endif(MUSE_APP_INSTALL_SUFFIX)

    set(CMAKE_INSTALL_RPATH "${QT_INSTALL_LIBS}")
    if (BUILD_SHARED_LIBS)
        set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH};${SHARED_LIBS_INSTALL_DESTINATION}")
    endif(BUILD_SHARED_LIBS)

elseif(OS_IS_MAC)

    set(MACOSX_BUNDLE_GUI_IDENTIFIER ${MUSE_APP_GUI_IDENTIFIER})
    set(MACOSX_BUNDLE_BUNDLE_NAME ${MUSE_APP_TITLE})
    set(MACOSX_BUNDLE_LONG_VERSION_STRING ${PIANOMANIA_MUSESCORE_PRODUCT_VERSION})
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PIANOMANIA_MUSESCORE_PRODUCT_VERSION})
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PIANOMANIA_MUSESCORE_PRODUCT_VERSION})
    set(MACOSX_BUNDLE_COPYRIGHT "Copyright © 1999-2026 MuseScore Limited. Published under the GNU General Public License version 3.")

    set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH};${QT_INSTALL_LIBS};@loader_path/../Frameworks")
    if (MUE_COMPILE_MACOS_PRECOMPILED_DEPS_PATH)
        list(PREPEND CMAKE_INSTALL_RPATH "${MUE_COMPILE_MACOS_PRECOMPILED_DEPS_PATH}/lib")
    endif()

    message("MACOSX_BUNDLE_VERSION: ${MACOSX_BUNDLE_LONG_VERSION_STRING}.${MACOSX_BUNDLE_BUNDLE_VERSION}")

elseif(OS_IS_WASM)

    # Emit the name the artifact is actually shipped under. Emscripten writes the
    # wasm filename into the generated .js, so renaming the pair afterwards leaves
    # the loader asking for a file that is no longer there.
    set(EXECUTABLE_NAME pm-converter)

    # Headless Pianomania converter link flags. The substantive flags (MODULARIZE,
    # EXPORT_NAME, FORCE_FILESYSTEM, INITIAL/MAXIMUM_MEMORY, ALLOW_MEMORY_GROWTH,
    # --embed-file, --bind) are owned by SetupBuildEnvironment.cmake. Here we only
    # set optimization and keep the zlib the .mscz reader needs. Deliberately NO
    # -g / ASSERTIONS / DEMANGLE_SUPPORT (debug info is the dominant size cost).
    set(EMCC_LINKER_FLAGS "\
        -O3 \
        -s USE_ZLIB=1 \
        -s EXIT_RUNTIME=0 \
        -s ASSERTIONS=0 \
        -s ALLOW_MEMORY_GROWTH=1 \
        -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
        --bind \
    ")

elseif(OS_IS_FBSD)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L /usr/local/lib -lasound -lpthread")

    set(CMAKE_INSTALL_RPATH "${QT_INSTALL_LIBS}")
    if (BUILD_SHARED_LIBS)
        set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_RPATH};${SHARED_LIBS_INSTALL_DESTINATION}")
    endif(BUILD_SHARED_LIBS)
else()
    message(FATAL_ERROR "Unsupported Platform: ${CMAKE_HOST_SYSTEM_NAME}")
endif()

###########################################
# Dependency library declaration
###########################################
set(LINK_LIB

    # Always needed by everyone
    ${QT_LIBRARIES}
    muse::global
    muse::draw
    )

macro(add_to_link_if_exists lib)
    if (TARGET ${lib})
        list(APPEND LINK_LIB ${lib})

        if (TARGET ${lib}_qml)
            list(APPEND LINK_LIB ${lib}_qml)
        endif()
    endif()
endmacro()

# NOTE There may be cases:
# 1. The module is enabled and the target exists.
# 2. The module is disabled, but there is a stub, the target exists
# 3. The module is disabled and the target does not exist.
# This function also adds qml parts to the link
add_to_link_if_exists(muse::accessibility)
add_to_link_if_exists(muse::actions)
add_to_link_if_exists(muse::audio)
add_to_link_if_exists(muse::audioplugins)
add_to_link_if_exists(muse::autobot)
add_to_link_if_exists(muse::cloud)
add_to_link_if_exists(muse::diagnostics)
add_to_link_if_exists(muse::dockwindow)
add_to_link_if_exists(muse::extensions)
add_to_link_if_exists(muse::languages)
add_to_link_if_exists(muse::learn)
add_to_link_if_exists(muse::media)
add_to_link_if_exists(muse::midi)
add_to_link_if_exists(muse::midiremote)
add_to_link_if_exists(muse::mpe)
add_to_link_if_exists(muse::multiwindows)
add_to_link_if_exists(muse::musesampler)
add_to_link_if_exists(muse::network)
add_to_link_if_exists(muse::shortcuts)
add_to_link_if_exists(muse::tours)
add_to_link_if_exists(muse::ui)
add_to_link_if_exists(muse::ui_dialogs_qml)
add_to_link_if_exists(muse::uicomponents)
add_to_link_if_exists(muse::uicomponents_qmlapi)
add_to_link_if_exists(muse::graphicaleffects_qml)
add_to_link_if_exists(muse::legacytreeview_qml)
add_to_link_if_exists(muse::update)
add_to_link_if_exists(muse::vst)
add_to_link_if_exists(muse::workspace)

add_to_link_if_exists(appshell)
add_to_link_if_exists(braille)
add_to_link_if_exists(context)
add_to_link_if_exists(converter)
add_to_link_if_exists(engraving)
add_to_link_if_exists(iex_audioexport)
add_to_link_if_exists(iex_bb)
add_to_link_if_exists(iex_bww)
add_to_link_if_exists(iex_capella)
add_to_link_if_exists(iex_guitarpro)
add_to_link_if_exists(iex_imagesexport)
add_to_link_if_exists(iex_lyricsexport)
add_to_link_if_exists(iex_mei)
add_to_link_if_exists(iex_midi)
add_to_link_if_exists(iex_mnx)
add_to_link_if_exists(iex_musedata)
add_to_link_if_exists(iex_musicxml)
add_to_link_if_exists(iex_ove)
add_to_link_if_exists(iex_tabledit)
add_to_link_if_exists(iex_videoexport)
add_to_link_if_exists(inspector)
add_to_link_if_exists(instrumentsscene)
add_to_link_if_exists(musesounds)
add_to_link_if_exists(notation)
add_to_link_if_exists(notationscene)
add_to_link_if_exists(palette)
add_to_link_if_exists(playback)
add_to_link_if_exists(preferences)
add_to_link_if_exists(print)
add_to_link_if_exists(project)     


set(MSCORE_APPEND_SRC)

if (OS_IS_WIN)
    set(MANIFEST_FILE_NAME "${EXECUTABLE_NAME}.manifest")
    configure_file("configs/app_win.manifest.in" ${MANIFEST_FILE_NAME} @ONLY)

    list(APPEND MSCORE_APPEND_SRC
        ${WINDOWS_ICONS_RC}
        "${CMAKE_CURRENT_BINARY_DIR}/${MANIFEST_FILE_NAME}"
    )
endif()

if (MUE_CONFIGURATION_IS_APPWEB)
    list(APPEND LINK_LIB appjs)
endif()

###########################################
# Resources
###########################################
qt_add_resources(APP_RCC_SOURCES app.qrc)
if (OS_IS_WIN)
    qt_add_resources(APP_RCC_SOURCES app_win.qrc)
endif()

###########################################
# Executable declaration
###########################################
set(APP_SRC
    main.cpp
    cmdoptions.h
    appfactory.cpp
    appfactory.h
)

if (NOT OS_IS_WASM)
    # Headless Pianomania wasm converter: only the console app path (GuiApp is GUI/threads).
    list(APPEND APP_SRC
        internal/guiapp.cpp
        internal/guiapp.h
    )
endif()

if (MUE_ENABLE_CONSOLEAPP)
    set(APP_SRC ${APP_SRC}
        internal/commandlineparser.cpp
        internal/commandlineparser.h
        internal/consoleapp.cpp
        internal/consoleapp.h
    )
endif()

if (CC_IS_EMCC AND MUE_CONFIGURATION_IS_APPWEB)
    set(APP_SRC ${APP_SRC} webapi_export.cpp)
endif()

qt_add_executable(MuseScoreStudio
    WIN32 MACOSX_BUNDLE
    ${APP_RCC_SOURCES}
    ${MSCORE_APPEND_SRC}
    ${APP_SRC}
)

###########################################
# Setting target properties
###########################################

set_target_properties(MuseScoreStudio PROPERTIES OUTPUT_NAME ${EXECUTABLE_NAME})

if (MUSE_ENABLE_UNIT_TESTS_CODE_COVERAGE)
    set(COVERAGE_FLAGS -fprofile-arcs -ftest-coverage --coverage)
    set(COVERAGE_LINK_FLAGS -lgcov --coverage)
    target_compile_options(MuseScoreStudio PRIVATE ${COVERAGE_FLAGS})
    target_link_options(MuseScoreStudio PRIVATE ${COVERAGE_LINK_FLAGS})
endif()

if (OS_IS_MAC)
    include(${PROJECT_SOURCE_DIR}/share/icons/SetupIconComposerIcon.cmake)
    target_setup_iconcomposer_icon(MuseScoreStudio "${PROJECT_SOURCE_DIR}/share/icons/AppIcon/AppIcon.icon")

    set_target_properties(MuseScoreStudio PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/buildscripts/packaging/macOS/Info.plist.in
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${MACOSX_BUNDLE_GUI_IDENTIFIER}
    )
endif (OS_IS_MAC)

if (CC_IS_MINGW)
    if (CMAKE_BUILD_TYPE MATCHES "DEBUG")
        set_target_properties(MuseScoreStudio PROPERTIES LINK_FLAGS "-mwindows -mconsole")
    else (CMAKE_BUILD_TYPE MATCHES "DEBUG")
        set_target_properties(MuseScoreStudio PROPERTIES LINK_FLAGS "-Wl,-S -mwindows")
    endif (CMAKE_BUILD_TYPE MATCHES "DEBUG")
endif(CC_IS_MINGW)

if (CC_IS_MSVC)
    target_link_options(MuseScoreStudio PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif(CC_IS_MSVC)

###########################################
# Includes
###########################################

target_include_directories(MuseScoreStudio PRIVATE
    ${PROJECT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}

    ${PROJECT_SOURCE_DIR}/src

    ${MUSE_FRAMEWORK_PATH}
    ${MUSE_FRAMEWORK_PATH}/framework
    ${MUSE_FRAMEWORK_PATH}/framework/global

    # compat
    ${MUSE_FRAMEWORK_PATH}/src
    ${MUSE_FRAMEWORK_PATH}/src/framework
    ${MUSE_FRAMEWORK_PATH}/src/framework/global
    # end compat
)

###########################################
# Link
###########################################

if (NOT OS_IS_WASM)
    target_link_libraries(MuseScoreStudio PRIVATE ${LINK_LIB} ${COVERAGE_LINK_FLAGS})
else()
    # Headless Pianomania wasm converter.
    # pmwasm objects are embedded directly into the app target (TARGET_OBJECTS
    # below), which also guarantees the EMSCRIPTEN_BINDINGS static initializers in
    # pmconverter.cpp survive linking (they would be dropped from a plain archive).
    list(APPEND LINK_LIB pmwasm)

    set(MSCORE_WASM_OBJ_SRC)
    foreach(link ${LINK_LIB})
        if (TARGET ${link})
            list(APPEND MSCORE_WASM_OBJ_SRC $<TARGET_OBJECTS:${link}>)
        endif()
    endforeach()

    # engraving's fonts are MODULE_BIG_QRC -> separate rcc_object_fonts_* OBJECT
    # targets that define qInitResources_fonts_*() (called by engravingmodule.cpp).
    # Nested TARGET_OBJECTS don't compose through $<TARGET_OBJECTS:engraving>, so
    # embed each one directly. The piano-only slim build declares only some of
    # these families (see src/engraving/CMakeLists.txt), so take the ones that
    # exist rather than assuming the full set, and name the ones left out. Naming
    # a family engraving never declared stops the generate step outright.
    foreach(font Academico Bravura Campania Edwin FreeSans FreeSerif Gootville Leland MScore
                 MuseJazz Petaluma FinaleMaestro FinaleBroadway Tabulature)
        if (TARGET rcc_object_fonts_${font})
            list(APPEND MSCORE_WASM_OBJ_SRC $<TARGET_OBJECTS:rcc_object_fonts_${font}>)
        else()
            list(APPEND MSCORE_WASM_FONTS_SKIPPED ${font})
        endif()
    endforeach()
    if (MSCORE_WASM_FONTS_SKIPPED)
        message(STATUS "wasm: fonts not embedded, not declared by engraving: ${MSCORE_WASM_FONTS_SKIPPED}")
    endif()

    target_sources(MuseScoreStudio PRIVATE ${MSCORE_WASM_OBJ_SRC})

    # No QML, so no QmlImportScanner / qml plugin import. MuseScore module objects
    # are embedded via TARGET_OBJECTS above; here we link Qt plus the thirdparty
    # static libs the modules need (freetype + harfbuzz for draw's font engine /
    # hb_ft_font_create, beatroot for iex_midi). freetype and harfbuzz reference
    # each other, so group them.
    target_link_libraries(MuseScoreStudio PRIVATE
        ${QT_LIBRARIES}
        -Wl,--start-group freetype harfbuzz -Wl,--end-group
        beatroot
    )

    set_target_properties(MuseScoreStudio PROPERTIES LINK_FLAGS "${EMCC_LINKER_FLAGS}")

    # Qt puts its own -sINITIAL_MEMORY on the link line through Qt6::Platform, and
    # it lands after ours, so the 128MB in SetupBuildEnvironment never took effect
    # and the link failed asking for more than Qt's 50MB default. Qt honours these
    # target properties, so set the same numbers where Qt will not overwrite them.
    set_target_properties(MuseScoreStudio PROPERTIES
        QT_WASM_INITIAL_MEMORY "134217728"
        QT_WASM_MAXIMUM_MEMORY "2147483648"
    )
endif()


###########################################
# INSTALL
###########################################

install(TARGETS MuseScoreStudio
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin
)

###########################################
# Windows
###########################################
if (OS_IS_WIN)
    include(GetCompilerInfo)

    if (CC_IS_MINGW)
        get_filename_component(COMPILER_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
        get_filename_component (MINGW_ROOT ${COMPILER_DIR} DIRECTORY)

        install( FILES
               ${MINGW_ROOT}/bin/libgcc_s_seh-1.dll
               ${MINGW_ROOT}/bin/libstdc++-6.dll
               ${MINGW_ROOT}/bin/libwinpthread-1.dll
               DESTINATION bin)

    endif(CC_IS_MINGW)

    if (SNDFILE_DLL)
        install(FILES ${SNDFILE_DLL} DESTINATION bin)
    endif()

    if (WIN_PORTABLE)
        # deploy the files and directory structure needed for the PortableApps.com format
        install(DIRECTORY ${PROJECT_SOURCE_DIR}/buildscripts/packaging/Windows/PortableApps/App DESTINATION ${CMAKE_INSTALL_PREFIX}/../..)
        install(DIRECTORY ${PROJECT_SOURCE_DIR}/buildscripts/packaging/Windows/PortableApps/Other DESTINATION ${CMAKE_INSTALL_PREFIX}/../..)
        install(FILES ${PROJECT_SOURCE_DIR}/buildscripts/packaging/Windows/PortableApps/help.html DESTINATION ${CMAKE_INSTALL_PREFIX}/../..)
        configure_file(${PROJECT_SOURCE_DIR}/buildscripts/packaging/Windows/PortableApps/appinfo.ini.in     ${CMAKE_INSTALL_PREFIX}/../../App/AppInfo/appinfo.ini   @ONLY)
    endif (WIN_PORTABLE)

    if (MUE_RUN_WINDEPLOYQT)
        set(deploy_tool_options_arg "--plugindir bin --qmldir ${PROJECT_SOURCE_DIR}")
        qt_generate_deploy_qml_app_script(
            TARGET MuseScoreStudio
            OUTPUT_SCRIPT deploy_script
            NO_UNSUPPORTED_PLATFORM_ERROR
            DEPLOY_TOOL_OPTIONS ${deploy_tool_options_arg}
        )

        install(SCRIPT ${deploy_script})
    endif() # MUE_RUN_WINDEPLOYQT

###########################################
# Linux
###########################################
elseif(OS_IS_LIN)
    # Install mscore executable (package maintainers may add "MuseScore" and/or "musescore" aliases that symlink to mscore)
    install(CODE "message(STATUS \"Creating symlink ${CMAKE_INSTALL_PREFIX}/bin/musescore${MUSE_APP_INSTALL_SUFFIX} -> ${CMAKE_INSTALL_PREFIX}/bin/mscore${MUSE_APP_INSTALL_SUFFIX}\")
                  execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink \"${CMAKE_INSTALL_PREFIX}/bin/mscore${MUSE_APP_INSTALL_SUFFIX}\" \"${CMAKE_INSTALL_PREFIX}/bin/musescore${MUSE_APP_INSTALL_SUFFIX}\")")

###########################################
# FreeBSD
###########################################
elseif(OS_IS_FBSD)
    # Install mscore executable (package maintainers may add "MuseScore" and/or "musescore" aliases that symlink to mscore)
    install(CODE "message(STATUS \"Creating symlink ${CMAKE_INSTALL_PREFIX}/bin/musescore${MUSE_APP_INSTALL_SUFFIX} -> ${CMAKE_INSTALL_PREFIX}/bin/mscore${MUSE_APP_INSTALL_SUFFIX}\")
                  execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink \"${CMAKE_INSTALL_PREFIX}/bin/mscore${MUSE_APP_INSTALL_SUFFIX}\" \"${CMAKE_INSTALL_PREFIX}/bin/musescore${MUSE_APP_INSTALL_SUFFIX}\")")

###########################################
# macOS
###########################################
elseif(OS_IS_MAC)
    # Pianomania: piano-only slim build — install only the font families the
    # slim engraving module embeds (see src/engraving/CMakeLists.txt).
    install(FILES
            ${PROJECT_SOURCE_DIR}/fonts/academico/AcademicoBold.otf
            ${PROJECT_SOURCE_DIR}/fonts/bravura/BravuraText.otf
            ${PROJECT_SOURCE_DIR}/fonts/campania/Campania.otf
            ${PROJECT_SOURCE_DIR}/fonts/edwin/Edwin-BdIta.otf
            ${PROJECT_SOURCE_DIR}/fonts/edwin/Edwin-Bold.otf
            ${PROJECT_SOURCE_DIR}/fonts/edwin/Edwin-Italic.otf
            ${PROJECT_SOURCE_DIR}/fonts/edwin/Edwin-Roman.otf
            ${PROJECT_SOURCE_DIR}/fonts/leland/Leland.otf
            ${PROJECT_SOURCE_DIR}/fonts/leland/LelandText.otf
            ${PROJECT_SOURCE_DIR}/fonts/mscore-BC.ttf
            ${PROJECT_SOURCE_DIR}/fonts/mscoreTab.ttf
            DESTINATION ${Mscore_SHARE_NAME}${Mscore_INSTALL_NAME}fonts
    )

    qt_generate_deploy_qml_app_script(
        TARGET MuseScoreStudio
        OUTPUT_SCRIPT deploy_script
        MACOS_BUNDLE_POST_BUILD
    )

###########################################
# Wasm
###########################################
elseif(OS_IS_WASM)

    # Headless Pianomania converter: we ship pm-converter.js/.wasm and load it from
    # our own webapp (see buildscripts/wasm/build.sh), so the Qt GUI HTML shell,
    # qtloader.js, and the upstream web js api installer are not needed.

else()
    message(FATAL_ERROR "Unsupported Platform: ${CMAKE_HOST_SYSTEM_NAME}")
endif()


#################################################
# Miscellaneous Microsoft Visual Studio settings
#################################################
if (MSVC)
    # Add a post-build event to deploy the application to the output folder. Same as building the INSTALL target.
    string(TOUPPER ${CMAKE_GENERATOR} UPPER_CMAKE_GENERATOR)
    if (${UPPER_CMAKE_GENERATOR} MATCHES "VISUAL STUDIO")
        # We want $(Configuration) passed verbatim. Note the round brackets.
        set(CONFIG_STR "$(Configuration)")

        add_custom_command(
            TARGET MuseScoreStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -DBUILD_TYPE=${CONFIG_STR} -P cmake_install.cmake
            WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
            VERBATIM
        )
    endif()

    # Force the "install" and "package" targets not to depend on the "all" target.
    set(CMAKE_SKIP_INSTALL_ALL_DEPENDENCY true)
    set(CMAKE_SKIP_PACKAGE_ALL_DEPENDENCY true)

    # Set the startup project to "MuseScoreStudio".
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT MuseScoreStudio)

    # Set the debugging properties for the "MuseScoreStudio" project.
    file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}/bin" VS_DEBUGGER_WORKING_DIRECTORY)
    set_target_properties(MuseScoreStudio PROPERTIES VS_DEBUGGER_COMMAND "${VS_DEBUGGER_WORKING_DIRECTORY}\\${EXECUTABLE_NAME}${CMAKE_EXECUTABLE_SUFFIX}")
    set_target_properties(MuseScoreStudio PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${VS_DEBUGGER_WORKING_DIRECTORY}")
    set_target_properties(MuseScoreStudio PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS "--debug")
endif (MSVC)
