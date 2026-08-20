# MIT License
#
# Copyright (c) 2018-2025 Jakub Melka and Contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

cmake_minimum_required(VERSION 3.16)

set(LOUPE_VERSION 0.1.0)
set(LOUPE_VERSION_PRERELEASE)
message(STATUS "Build type - CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

project(LOUPE VERSION ${LOUPE_VERSION} LANGUAGES CXX)

# Appx Identity.Version requires four components. MSI accepts SemVer three-part.
set(LOUPE_WINDOWS_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")
if(LOUPE_VERSION_PRERELEASE)
    set(LOUPE_PROJECT_VERSION "${LOUPE_VERSION}-${LOUPE_VERSION_PRERELEASE}")
else()
    set(LOUPE_PROJECT_VERSION "${LOUPE_VERSION}")
endif()

message("VCPKG Enabled: " ${VCPKG_TOOLCHAIN})

option(LOUPE_INSTALL_DEPENDENCIES "Install dependencies" ${VCPKG_TOOLCHAIN})
option(LOUPE_INSTALL_QT_DEPENDENCIES "Install Qt dependencies" ${VCPKG_TOOLCHAIN})
option(LOUPE_INSTALL_INCLUDE "Install include headers" ON)
option(LOUPE_FLATPAK_BUILD "Special flag for Flatpak" OFF)

add_compile_definitions(LOUPE_PROJECT_VERSION="${LOUPE_PROJECT_VERSION}")
add_compile_definitions(QT_NO_EMIT)

if(LOUPE_FLATPAK_BUILD)
    add_compile_definitions(LOUPE_FLATPAK_BUILD)
endif()

if(WIN32 AND MSVC)
    option(LOUPE_INSTALL_MSVC_REDISTRIBUTABLE "Install MSVC redistributable package" ON)
    option(LOUPE_INSTALL_PREPARE_WIX_INSTALLER "Prepare Wix installer for Windows" ON)
endif()

option(LOUPE_BUILD_ONLY_CORE_LIBRARY "Build only core library" OFF)
option(LOUPE_BUILD_TESTS "Build UnitTests" ON)
option(LOUPE_BUILD_QUICK_SHELL_SMOKE "Build the Qt Quick shell backend smoke harness" OFF)
option(LOUPE_BUILD_CANVAS_BENCHMARK "Build the Qt Quick canvas-hosting benchmark" OFF)
option(LOUPE_ENABLE_SANITIZERS "Build with ASan+UBSan" OFF)
option(LOUPE_BUILD_FUZZERS "Build libFuzzer harnesses" OFF)
if(WIN32)
    set(_LOUPE_ENABLE_SENTRY_DEFAULT ON)
else()
    set(_LOUPE_ENABLE_SENTRY_DEFAULT OFF)
endif()
option(LOUPE_ENABLE_SENTRY "Enable Sentry Native crash reporting (Windows-first)" ${_LOUPE_ENABLE_SENTRY_DEFAULT})
# Public ingest DSN for berry-studios/loupe-pdf (EU). Runtime SENTRY_DSN overrides;
# SENTRY_DSN=off disables telemetry. CI jobs set SENTRY_DSN=off so builds do not
# flood the project. Confidential workflows should also set SENTRY_DSN=off.
set(_LOUPE_SENTRY_DSN_DEFAULT "https://44f65b76ce476befdfb321ca361921fe@o4511731410337792.ingest.de.sentry.io/4511866328449104")
set(LOUPE_SENTRY_DSN "${_LOUPE_SENTRY_DSN_DEFAULT}" CACHE STRING "Compile-time Sentry DSN (set SENTRY_DSN=off at runtime to disable)")
if(LOUPE_SENTRY_DSN STREQUAL "")
    set(LOUPE_SENTRY_DSN "${_LOUPE_SENTRY_DSN_DEFAULT}" CACHE STRING "Compile-time Sentry DSN (set SENTRY_DSN=off at runtime to disable)" FORCE)
endif()

if(LOUPE_ENABLE_SENTRY)
    find_package(Threads REQUIRED)
    find_package(sentry CONFIG REQUIRED)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/LoupeSentry.cmake)
endif()

# Loupe release profile: retain the standalone production/developer tools while
# deferring only explicitly out-of-profile plugins without deleting sources.
option(LOUPE_LOUPE_DISTRIBUTION "Loupe release profile" OFF)

# 0.2B keeps compatibility executables in the install tree but presents one
# desktop product. Full developer packages retain the inherited application
# registrations for direct testing and upstream comparison.
if(LOUPE_LOUPE_DISTRIBUTION)
    set(LOUPE_LOUPE_COMPAT_APPLICATIONS "")
else()
    set(LOUPE_LOUPE_COMPAT_APPLICATIONS [=[
	<Application Id="LoupeViewer" Executable="LoupeViewer.exe" EntryPoint="windows.fullTrustApplication">
      <uap:VisualElements DisplayName="Loupe Viewer"
						  Description="View and navigate PDF documents."
						  BackgroundColor="transparent"
						  Square150x150Logo="assets\150x150\io.github.mberrys.Loupe-pdf.LoupeViewer.png"
						  Square44x44Logo="assets\44x44\io.github.mberrys.Loupe-pdf.LoupeViewer.png">
      </uap:VisualElements>
    </Application>
	<Application Id="LoupePageMaster" Executable="LoupePageMaster.exe" EntryPoint="windows.fullTrustApplication">
      <uap:VisualElements DisplayName="Loupe PageMaster"
						  Description="Organize, merge, split, and duplicate PDF pages."
						  BackgroundColor="transparent"
						  Square150x150Logo="assets\150x150\io.github.mberrys.Loupe-pdf.LoupePageMaster.png"
						  Square44x44Logo="assets\44x44\io.github.mberrys.Loupe-pdf.LoupePageMaster.png">
      </uap:VisualElements>
    </Application>
	<Application Id="LoupeDiff" Executable="LoupeDiff.exe" EntryPoint="windows.fullTrustApplication">
      <uap:VisualElements DisplayName="Loupe Diff"
						  Description="Compare PDF documents."
						  BackgroundColor="transparent"
						  Square150x150Logo="assets\150x150\io.github.mberrys.Loupe-pdf.LoupeDiff.png"
						  Square44x44Logo="assets\44x44\io.github.mberrys.Loupe-pdf.LoupeDiff.png">
      </uap:VisualElements>
    </Application>
	<Application Id="LoupeLaunchPad" Executable="LoupeLaunchPad.exe" EntryPoint="windows.fullTrustApplication">
      <uap:VisualElements DisplayName="Loupe LaunchPad"
						  Description="Launch Loupe applications."
						  BackgroundColor="transparent"
						  Square150x150Logo="assets\logo.png"
						  Square44x44Logo="assets\logo.png">
      </uap:VisualElements>
    </Application>]=])
endif()

if(LOUPE_LOUPE_DISTRIBUTION)
    set(_LOUPE_BUILD_CODE_GENERATOR_DEFAULT ON)
    set(_LOUPE_BUILD_JBIG2_VIEWER_DEFAULT ON)
    set(_LOUPE_BUILD_EXAMPLE_GENERATOR_DEFAULT ON)
    set(_LOUPE_BUILD_VIEWER_DEFAULT ON)
    set(_LOUPE_BUILD_PAGEMASTER_DEFAULT ON)
    set(_LOUPE_BUILD_DIFF_DEFAULT ON)
    set(_LOUPE_BUILD_LAUNCHPAD_DEFAULT ON)
    set(_LOUPE_PLUGIN_AUDIOBOOK_DEFAULT OFF)
    set(_LOUPE_PLUGIN_SCANNER_DEFAULT ON)
    set(_LOUPE_PLUGIN_OCR_DEFAULT OFF)
else()
    set(_LOUPE_BUILD_CODE_GENERATOR_DEFAULT ON)
    set(_LOUPE_BUILD_JBIG2_VIEWER_DEFAULT ON)
    set(_LOUPE_BUILD_EXAMPLE_GENERATOR_DEFAULT ON)
    set(_LOUPE_BUILD_VIEWER_DEFAULT ON)
    set(_LOUPE_BUILD_PAGEMASTER_DEFAULT ON)
    set(_LOUPE_BUILD_DIFF_DEFAULT ON)
    set(_LOUPE_BUILD_LAUNCHPAD_DEFAULT ON)
    set(_LOUPE_PLUGIN_AUDIOBOOK_DEFAULT ON)
    set(_LOUPE_PLUGIN_SCANNER_DEFAULT ON)
    set(_LOUPE_PLUGIN_OCR_DEFAULT ON)
endif()

option(LOUPE_BUILD_CODE_GENERATOR "Build CodeGenerator developer tool" ${_LOUPE_BUILD_CODE_GENERATOR_DEFAULT})
option(LOUPE_BUILD_JBIG2_VIEWER "Build JBIG2_Viewer developer tool" ${_LOUPE_BUILD_JBIG2_VIEWER_DEFAULT})
option(LOUPE_BUILD_EXAMPLE_GENERATOR "Build PdfExampleGenerator developer tool" ${_LOUPE_BUILD_EXAMPLE_GENERATOR_DEFAULT})
option(LOUPE_BUILD_VIEWER "Build LoupeViewer application" ${_LOUPE_BUILD_VIEWER_DEFAULT})
option(LOUPE_BUILD_PAGEMASTER "Build LoupePageMaster application" ${_LOUPE_BUILD_PAGEMASTER_DEFAULT})
option(LOUPE_BUILD_DIFF "Build LoupeDiff application" ${_LOUPE_BUILD_DIFF_DEFAULT})
option(LOUPE_BUILD_LAUNCHPAD "Build LoupeLaunchPad application" ${_LOUPE_BUILD_LAUNCHPAD_DEFAULT})
option(LOUPE_PLUGIN_AUDIOBOOK "Build AudioBook editor plugin" ${_LOUPE_PLUGIN_AUDIOBOOK_DEFAULT})
option(LOUPE_PLUGIN_SCANNER "Build Scanner editor plugin" ${_LOUPE_PLUGIN_SCANNER_DEFAULT})

# V1 ships OCR as CLI-only (PdfTool ocr) per the 2026-07-25 product decision (MIC-343).
# The Editor OCR UI plugin is not part of the V1 release surface, but stays buildable
# for local/developer use. Release packaging workflows pass -DLOUPE_PLUGIN_OCR=OFF
# explicitly; PdfTool's OCR subcommand is unaffected. (LOUPE_BUNDLE_OCR_SERVICE, the
# other half of the V1 CLI-only OCR surface, is declared in loupe-ocr/tools/CMakeLists.txt.)
option(LOUPE_PLUGIN_OCR "Build the OCR editor plugin (UI)" ${_LOUPE_PLUGIN_OCR_DEFAULT})

if(LOUPE_LOUPE_DISTRIBUTION AND LOUPE_PLUGIN_OCR)
    message(FATAL_ERROR
        "LOUPE_PLUGIN_OCR=ON is incompatible with the Loupe release profile: "
        "OCR is CLI-only in V1 (see issue #41 and ADR-005).")
endif()

if(LOUPE_ENABLE_SANITIZERS OR LOUPE_BUILD_FUZZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(LOUPE_QT_ROOT "" CACHE PATH "Qt root directory")

if(VCPKG_TOOLCHAIN AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/install CACHE PATH "Installation directory" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(Qt6 6.11.1 REQUIRED COMPONENTS Core LinguistTools)

if(LOUPE_BUILD_ONLY_CORE_LIBRARY)
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Core Gui Svg Xml Sql)
else()
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Core Gui Widgets Svg Xml Sql PrintSupport TextToSpeech Concurrent)
endif()

if(LOUPE_BUILD_TESTS)
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Test)
endif()

if(LOUPE_BUILD_QUICK_SHELL_SMOKE OR LOUPE_BUILD_CANVAS_BENCHMARK)
    if(LOUPE_BUILD_ONLY_CORE_LIBRARY)
        message(FATAL_ERROR "Qt Quick qualification targets require a GUI build")
    endif()
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Qml Quick QuickControls2)
endif()

if(LOUPE_BUILD_CANVAS_BENCHMARK)
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS QuickWidgets)
endif()

qt_standard_project_setup(I18N_TRANSLATED_LANGUAGES en de cs es ko zh_CN zh_TW fr tr ru)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Freetype REQUIRED)
find_package(OpenJPEG CONFIG REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(blend2d CONFIG REQUIRED)

if(VCPKG_TOOLCHAIN)
    find_package(lcms2 REQUIRED)
    set(LCMS2_LIBRARIES lcms2::lcms2 CACHE INTERNAL "LCMS2 libraries")
else() # lcms usually doesn't provide a config file
    find_library(LCMS2_LIBRARIES lcms2 REQUIRED)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_AUTORCC_OPTIONS "--threshold;0;--compress;9")

if (MSVC)
    add_compile_options(/bigobj /W4 /wd5054 /wd4127 /wd4702)
    if(LOUPE_ENABLE_SENTRY)
        # Crashpad minidumps need PDBs to symbolicate. Keep Release optimization.
        add_compile_options("$<$<CONFIG:Release>:/Zi>")
        add_link_options("$<$<CONFIG:Release>:/DEBUG:FULL>"
                         "$<$<CONFIG:Release>:/OPT:REF>"
                         "$<$<CONFIG:Release>:/OPT:ICF>")
    endif()
endif()

if (MINGW)
    add_compile_options("-Wa,-mbig-obj")
endif()

if (UNIX AND NOT APPLE AND CMAKE_COMPILER_IS_GNUCXX)
    set(LINUX_GCC ON)
endif()

if (UNIX AND NOT APPLE)
    set(LOUPE_LINUX ON)
endif()

# Parallel STL (libstdc++/libc++) on Linux needs TBB for both GCC and Clang.
if(LOUPE_LINUX)
    find_package(TBB CONFIG REQUIRED)
    set(LOUPE_USE_TBB ON)
endif()

if(LOUPE_ENABLE_SENTRY)
    find_package(sentry CONFIG REQUIRED)
endif()

option(LOUPE_INSTALL_TO_USR "Install to usr directory" ${VCPKG_TOOLCHAIN})

if(LOUPE_LINUX)
    set(LOUPE_INSTALL_LIB_DIR_IMPL ${CMAKE_INSTALL_LIBDIR})
    set(LOUPE_INSTALL_BIN_DIR_IMPL ${CMAKE_INSTALL_BINDIR})
else()
    set(LOUPE_INSTALL_LIB_DIR_IMPL ${CMAKE_INSTALL_BINDIR})
    set(LOUPE_INSTALL_BIN_DIR_IMPL ${CMAKE_INSTALL_BINDIR})
endif()

if(LOUPE_INSTALL_TO_USR)
    set(LOUPE_INSTALL_LIB_DIR usr/${LOUPE_INSTALL_LIB_DIR_IMPL})
    set(LOUPE_INSTALL_BIN_DIR usr/${LOUPE_INSTALL_BIN_DIR_IMPL})
    set(LOUPE_INSTALL_SHARE_DIR usr/${CMAKE_INSTALL_DATAROOTDIR})
    set(LOUPE_INSTALL_INCLUDE_DIR usr/${CMAKE_INSTALL_INCLUDEDIR})
else()
    set(LOUPE_INSTALL_LIB_DIR ${LOUPE_INSTALL_LIB_DIR_IMPL})
    set(LOUPE_INSTALL_BIN_DIR ${LOUPE_INSTALL_BIN_DIR_IMPL})
    set(LOUPE_INSTALL_SHARE_DIR ${CMAKE_INSTALL_DATAROOTDIR})
    set(LOUPE_INSTALL_INCLUDE_DIR ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(LOUPE_LINUX)
    set(LOUPE_PLUGINS_DIR ${LOUPE_INSTALL_LIB_DIR}/loupe)
    set(LOUPE_INSTALL_TRANS_DIR ${LOUPE_INSTALL_SHARE_DIR}/loupe/translations)
else()
    set(LOUPE_PLUGINS_DIR ${LOUPE_INSTALL_LIB_DIR}/pdfplugins)
    set(LOUPE_INSTALL_TRANS_DIR ${LOUPE_INSTALL_BIN_DIR}/translations)
endif()

# Relative paths are used by the config
cmake_path(RELATIVE_PATH LOUPE_PLUGINS_DIR
           BASE_DIRECTORY "${LOUPE_INSTALL_BIN_DIR}"
           OUTPUT_VARIABLE LOUPE_PLUGINS_RELATIVE_PATH)
cmake_path(RELATIVE_PATH LOUPE_INSTALL_TRANS_DIR
           BASE_DIRECTORY "${LOUPE_INSTALL_BIN_DIR}"
           OUTPUT_VARIABLE LOUPE_TRANSLATIONS_RELATIVE_PATH)
configure_file(config.h.cmake config.h)

add_subdirectory(LoupeLibCore)

if(LOUPE_INSTALL_INCLUDE)
    # Install public headers from LoupeLibCore/include
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/LoupeLibCore/sources/
            DESTINATION ${LOUPE_INSTALL_INCLUDE_DIR}/LoupeLibCore
            FILES_MATCHING PATTERN "*.h")

    # Install generated export header
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/loupelibcore_export.h
            DESTINATION ${LOUPE_INSTALL_INCLUDE_DIR}/LoupeLibCore)

    # Install public headers from LoupeLibWidgets/include
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/LoupeLibWidgets/sources/
            DESTINATION ${LOUPE_INSTALL_INCLUDE_DIR}/LoupeLibWidgets
            FILES_MATCHING PATTERN "*.h")

    # Install generated export header
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/loupelibwidgets_export.h
            DESTINATION ${LOUPE_INSTALL_INCLUDE_DIR}/LoupeLibWidgets)
endif()

if(NOT LOUPE_BUILD_ONLY_CORE_LIBRARY)
    add_subdirectory(LoupeLibWidgets)
    if(LOUPE_BUILD_CODE_GENERATOR)
        add_subdirectory(CodeGenerator)
    endif()
    if(LOUPE_BUILD_JBIG2_VIEWER)
        add_subdirectory(JBIG2_Viewer)
    endif()
    if(LOUPE_BUILD_EXAMPLE_GENERATOR)
        add_subdirectory(PdfExampleGenerator)
    endif()
    add_subdirectory(PdfTool)
    add_subdirectory(loupe-preflight/tools)
    add_subdirectory(loupe-ocr/tools)
    add_subdirectory(LoupeLibGui)
    if(LOUPE_BUILD_QUICK_SHELL_SMOKE)
        add_subdirectory(QuickShellSmoke)
    endif()
    if(LOUPE_BUILD_CANVAS_BENCHMARK)
        add_subdirectory(CanvasBenchmark)
    endif()
    add_subdirectory(LoupeEditorPlugins)
    add_subdirectory(LoupeEditor)
    if(LOUPE_BUILD_VIEWER)
        add_subdirectory(LoupeViewer)
    endif()
    if(LOUPE_BUILD_PAGEMASTER)
        add_subdirectory(LoupePageMaster)
    endif()
    if(LOUPE_BUILD_DIFF)
        add_subdirectory(LoupeDiff)
    endif()
    if(LOUPE_BUILD_LAUNCHPAD)
        add_subdirectory(LoupeLaunchPad)
    endif()
    add_subdirectory(WixInstaller)
endif()

if(LOUPE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(UnitTests)
endif()

if(LOUPE_BUILD_FUZZERS)
    add_subdirectory(Fuzz)
endif()

qt_collect_translation_source_targets(i18n_targets)
message("CMAKE_TRANSLATION_TARGETS = " ${i18n_targets})

set(LOUPE_TRANSLATION_TS_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/translations)
set(LOUPE_TRANSLATION_QM_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_TRANS_DIR})

message("LOUPE_TRANSLATION_TS_OUTPUT_DIRECTORY" ${LOUPE_TRANSLATION_TS_OUTPUT_DIRECTORY})
message("LOUPE_TRANSLATION_QM_OUTPUT_DIRECTORY" ${LOUPE_TRANSLATION_QM_OUTPUT_DIRECTORY})

# Only add translation targets for targets that actually exist in this build.
foreach(_tgt LoupeEditor LoupeViewer LoupePageMaster LoupeDiff PdfTool)
    if(TARGET ${_tgt})
        list(APPEND _existing_translation_targets ${_tgt})
    endif()
endforeach()
if(_existing_translation_targets)
    qt_add_translations(TARGETS ${_existing_translation_targets}
                        SOURCE_TARGETS ${i18n_targets}
                        MERGE_QT_TRANSLATIONS
                        LUPDATE_OPTIONS -no-obsolete
                        TS_OUTPUT_DIRECTORY ${LOUPE_TRANSLATION_TS_OUTPUT_DIRECTORY}
                        QM_OUTPUT_DIRECTORY ${LOUPE_TRANSLATION_QM_OUTPUT_DIRECTORY}
                        QM_FILES_OUTPUT_VARIABLE LOUPE_QM_FILES)
endif()

if(WIN32)
    set(LOUPE_LUPDATE_TARGET "${PROJECT_NAME}_lupdate")
    if(TARGET ${LOUPE_LUPDATE_TARGET})
        add_custom_command(TARGET ${LOUPE_LUPDATE_TARGET}
                           POST_BUILD
                           COMMAND ${CMAKE_COMMAND}
                                   -DTS_OUTPUT_DIRECTORY=${LOUPE_TRANSLATION_TS_OUTPUT_DIRECTORY}
                                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NormalizeTsLineEndings.cmake
                           COMMENT "Normalizing TS files to CRLF")
    endif()
endif()

message("CMAKE_PREFIX_PATH = " ${CMAKE_PREFIX_PATH})
message("CMAKE_TOOLCHAIN_FILE = " ${CMAKE_TOOLCHAIN_FILE})

configure_file(version.txt.in version.txt)

if(NOT LOUPE_BUILD_ONLY_CORE_LIBRARY)
    install(FILES ${LOUPE_QM_FILES}
            DESTINATION ${LOUPE_INSTALL_TRANS_DIR})

    if(WIN32)
        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Desktop/44x44/
                RUNTIME DESTINATION ${LOUPE_INSTALL_BIN_DIR}/assets/44x44
                FILES_MATCHING
                PATTERN *.png)
        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Desktop/150x150/
                RUNTIME DESTINATION ${LOUPE_INSTALL_BIN_DIR}/assets/150x150
                FILES_MATCHING
                PATTERN *.png)

        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/AppxManifest.xml.in ${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml DESTINATION ${LOUPE_INSTALL_BIN_DIR})
        install(FILES assets/logo.png DESTINATION ${LOUPE_INSTALL_BIN_DIR}/assets)
    else()
        configure_file(make-package.sh.in make-package.sh)

        if(LOUPE_LOUPE_DISTRIBUTION)
            set(_LOUPE_DESKTOP_ENTRIES
                Desktop/io.github.mberrys.Loupe-pdf.desktop)
            set(_LOUPE_DESKTOP_ICONS
                Desktop/io.github.mberrys.Loupe-pdf.svg)
            set(_LOUPE_DESKTOP_PNGS
                Desktop/128x128/io.github.mberrys.Loupe-pdf.png)
        else()
            set(_LOUPE_DESKTOP_ENTRIES
                Desktop/io.github.mberrys.Loupe-pdf.LoupeDiff.desktop
                Desktop/io.github.mberrys.Loupe-pdf.LoupePageMaster.desktop
                Desktop/io.github.mberrys.Loupe-pdf.LoupeViewer.desktop
                Desktop/io.github.mberrys.Loupe-pdf.LoupeEditor.desktop
                Desktop/io.github.mberrys.Loupe-pdf.desktop)
            set(_LOUPE_DESKTOP_ICONS
                Desktop/io.github.mberrys.Loupe-pdf.LoupeDiff.svg
                Desktop/io.github.mberrys.Loupe-pdf.LoupePageMaster.svg
                Desktop/io.github.mberrys.Loupe-pdf.LoupeViewer.svg
                Desktop/io.github.mberrys.Loupe-pdf.LoupeEditor.svg
                Desktop/io.github.mberrys.Loupe-pdf.svg)
            set(_LOUPE_DESKTOP_PNGS
                Desktop/128x128/io.github.mberrys.Loupe-pdf.LoupeDiff.png
                Desktop/128x128/io.github.mberrys.Loupe-pdf.LoupePageMaster.png
                Desktop/128x128/io.github.mberrys.Loupe-pdf.LoupeViewer.png
                Desktop/128x128/io.github.mberrys.Loupe-pdf.LoupeEditor.png
                Desktop/128x128/io.github.mberrys.Loupe-pdf.png)
        endif()

        install(FILES ${_LOUPE_DESKTOP_ENTRIES}
                      DESTINATION ${LOUPE_INSTALL_SHARE_DIR}/applications)
        install(FILES ${_LOUPE_DESKTOP_ICONS}
                      DESTINATION ${LOUPE_INSTALL_SHARE_DIR}/icons/hicolor/scalable/apps)
        install(FILES Desktop/io.github.mberrys.Loupe-pdf.appdata.xml DESTINATION ${LOUPE_INSTALL_SHARE_DIR}/metainfo)
        install(FILES ${_LOUPE_DESKTOP_PNGS}
                      DESTINATION ${LOUPE_INSTALL_SHARE_DIR}/icons/hicolor/128x128/apps)
    endif()

endif()

if(LOUPE_INSTALL_DEPENDENCIES AND NOT LOUPE_BUILD_ONLY_CORE_LIBRARY)
    install(DIRECTORY ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/
        RUNTIME DESTINATION ${LOUPE_INSTALL_LIB_DIR}/
        FILES_MATCHING
        PATTERN "*.dll"
        PATTERN "*.so"
        PATTERN "*.dylib"
    )

    if("${LOUPE_QT_ROOT}" STREQUAL "")
        message(WARNING "Set variable LOUPE_QT_ROOT to Qt6 root directory")
    endif()

    if(LOUPE_INSTALL_QT_DEPENDENCIES)
        install(DIRECTORY ${LOUPE_QT_ROOT}/bin/
            RUNTIME DESTINATION ${LOUPE_INSTALL_LIB_DIR}/
            FILES_MATCHING
            REGEX "(Qt6Core|Qt6Gui|Qt6PrintSupport|Qt6Svg|Qt6TextToSpeech|Qt6Widgets|Qt6Xml|Qt6Multimedia|Qt6Network)\\..*"
            PATTERN "Debug" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
        )

        # ICU is a load-time dependency of Qt6Core on Windows aqt builds; without
        # these DLLs beside PdfTool.exe the MSI-installed binary fails with
        # STATUS_DLL_NOT_FOUND (0xC0000135) even when Qt6*.dll themselves are present.
        install(DIRECTORY ${LOUPE_QT_ROOT}/bin/
            RUNTIME DESTINATION ${LOUPE_INSTALL_LIB_DIR}/
            FILES_MATCHING
            PATTERN "icu*.dll"
            PATTERN "Debug" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
        )

        install(DIRECTORY ${LOUPE_QT_ROOT}/plugins/platforms/
            RUNTIME DESTINATION ${LOUPE_INSTALL_LIB_DIR}/platforms/
            FILES_MATCHING
            PATTERN "qwindows.dll"
            PATTERN "qoffscreen.dll"
            PATTERN "*.so"
            PATTERN "*.dylib"
            PATTERN "*.pdb" EXCLUDE
        )

        install(DIRECTORY ${LOUPE_QT_ROOT}/plugins/iconengines/
            RUNTIME DESTINATION ${LOUPE_INSTALL_LIB_DIR}/iconengines/
            FILES_MATCHING
            REGEX "qsvgicon\\..*"
            PATTERN "*.pdb" EXCLUDE
        )

        install(DIRECTORY ${LOUPE_QT_ROOT}/plugins/imageformats/
            RUNTIME DESTINATION ${LOUPE_INSTALL_LIB_DIR}/imageformats/
            FILES_MATCHING
            PATTERN "*.dll"
            PATTERN "*.so"
            PATTERN "*.dylib"
            REGEX "d\\..*" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
        )

        install(DIRECTORY ${LOUPE_QT_ROOT}/plugins/styles/
            RUNTIME DESTINATION ${LOUPE_INSTALL_LIB_DIR}/styles/
            FILES_MATCHING
            PATTERN "*.dll"
            PATTERN "*.so"
            PATTERN "*.dylib"
            REGEX "d\\..*" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
        )

        install(DIRECTORY ${LOUPE_QT_ROOT}/plugins/texttospeech/
            RUNTIME DESTINATION ${LOUPE_INSTALL_LIB_DIR}/texttospeech/
            FILES_MATCHING
            PATTERN "*.dll"
            PATTERN "*.so"
            PATTERN "*.dylib"
            REGEX "d\\..*" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
        )
    endif()

    if(WIN32 AND MSVC AND LOUPE_INSTALL_MSVC_REDISTRIBUTABLE)
        if(DEFINED ENV{MSVC_REDISTRIBUTABLES_PATH})
            set(MSVC_REDISTRIBUTABLES_PATH $ENV{MSVC_REDISTRIBUTABLES_PATH})
            message(STATUS "Using existing MSVC Redistributable Package Path = ${MSVC_REDISTRIBUTABLES_PATH}")
        else()
            get_filename_component(MSVC_REDISTRIBUTABLES_PATH $ENV{VCToolsRedistDir}/$ENV{VSCMD_ARG_TGT_ARCH}/Microsoft.VC${MSVC_TOOLSET_VERSION}.CRT/ ABSOLUTE)
            message(STATUS "MSVC Redistributable Package Path = ${MSVC_REDISTRIBUTABLES_PATH}")
        endif()

        install(DIRECTORY ${MSVC_REDISTRIBUTABLES_PATH}/
            RUNTIME DESTINATION ${LOUPE_INSTALL_LIB_DIR}/
            FILES_MATCHING
            PATTERN "*.dll"
        )
    endif()
endif()

if(LOUPE_ENABLE_SENTRY AND TARGET sentry_crashpad::crashpad_handler)
    get_target_property(_loupe_sentry_crashpad_handler_install sentry_crashpad::crashpad_handler IMPORTED_LOCATION_RELEASE)
    if(NOT _loupe_sentry_crashpad_handler_install)
        get_target_property(_loupe_sentry_crashpad_handler_install sentry_crashpad::crashpad_handler IMPORTED_LOCATION)
    endif()

    if(_loupe_sentry_crashpad_handler_install)
        if(WIN32)
            install(PROGRAMS ${_loupe_sentry_crashpad_handler_install}
                    DESTINATION ${LOUPE_INSTALL_BIN_DIR}
                    RENAME crashpad_handler.exe)
        else()
            install(PROGRAMS ${_loupe_sentry_crashpad_handler_install}
                    DESTINATION ${LOUPE_INSTALL_BIN_DIR}
                    RENAME crashpad_handler)
        endif()
    endif()

    if(TARGET sentry_crashpad::crashpad_wer)
        get_target_property(_loupe_sentry_crashpad_wer_install sentry_crashpad::crashpad_wer IMPORTED_LOCATION_RELEASE)
        if(NOT _loupe_sentry_crashpad_wer_install)
            get_target_property(_loupe_sentry_crashpad_wer_install sentry_crashpad::crashpad_wer IMPORTED_LOCATION)
        endif()

        if(_loupe_sentry_crashpad_wer_install)
            install(FILES ${_loupe_sentry_crashpad_wer_install}
                    DESTINATION ${LOUPE_INSTALL_BIN_DIR})
        endif()
    endif()
endif()

#macro(dump_variables)
#    message(STATUS "dump_variables------------------------------------------{")
#    get_cmake_property(_variableNames VARIABLES)
#    foreach (_variableName ${_variableNames})
#        message(STATUS "${_variableName}=${${_variableName}}")
#    endforeach()
#    message(STATUS "dump_variables------------------------------------------}")
#endmacro()
#
#dump_variables()
