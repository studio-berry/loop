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

set(LOOP_VERSION 0.2.0)
set(LOOP_VERSION_PRERELEASE alpha)
set(LOOP_PRODUCT_NAME "Loop")
set(LOOP_ORGANIZATION_NAME "Loop")
set(LOOP_ORGANIZATION_DOMAIN "io.github.mberrys")
set(LOOP_PACKAGE_ID "io.github.mberrys.Loop-pdf")
message(STATUS "Build type - CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

project(LOOP VERSION ${LOOP_VERSION} LANGUAGES CXX)

# Appx Identity.Version requires four components. MSI accepts SemVer three-part.
set(LOOP_WINDOWS_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")
if(LOOP_VERSION_PRERELEASE)
    set(LOOP_PROJECT_VERSION "${LOOP_VERSION}-${LOOP_VERSION_PRERELEASE}")
else()
    set(LOOP_PROJECT_VERSION "${LOOP_VERSION}")
endif()

message("VCPKG Enabled: " ${VCPKG_TOOLCHAIN})

option(LOOP_INSTALL_DEPENDENCIES "Install dependencies" ${VCPKG_TOOLCHAIN})
option(LOOP_INSTALL_QT_DEPENDENCIES "Install Qt dependencies" ${VCPKG_TOOLCHAIN})
option(LOOP_INSTALL_INCLUDE "Install include headers" ON)
option(LOOP_FLATPAK_BUILD "Special flag for Flatpak" OFF)

add_compile_definitions(LOOP_PROJECT_VERSION="${LOOP_PROJECT_VERSION}")
add_compile_definitions(LOOP_PRODUCT_NAME="${LOOP_PRODUCT_NAME}")
add_compile_definitions(LOOP_ORGANIZATION_NAME="${LOOP_ORGANIZATION_NAME}")
add_compile_definitions(LOOP_ORGANIZATION_DOMAIN="${LOOP_ORGANIZATION_DOMAIN}")
add_compile_definitions(LOOP_PACKAGE_ID="${LOOP_PACKAGE_ID}")
add_compile_definitions(QT_NO_EMIT)

if(LOOP_FLATPAK_BUILD)
    add_compile_definitions(LOOP_FLATPAK_BUILD)
endif()

if(WIN32 AND MSVC)
    option(LOOP_INSTALL_MSVC_REDISTRIBUTABLE "Install MSVC redistributable package" ON)
    option(LOOP_INSTALL_PREPARE_WIX_INSTALLER "Prepare Wix installer for Windows" ON)
endif()

option(LOOP_BUILD_ONLY_CORE_LIBRARY "Build only core library" OFF)
option(LOOP_BUILD_TESTS "Build UnitTests" ON)
option(LOOP_BUILD_QUICK_SHELL_SMOKE "Build the Qt Quick shell backend smoke harness (S22 qualification-only, non-installed)" OFF)
option(LOOP_BUILD_PRODUCT_QUICK_A11Y_SMOKE "Build the product Quick accessibility smoke harness (P4-S10)" OFF)
option(LOOP_BUILD_CANVAS_BENCHMARK "Build the Qt Quick canvas-hosting benchmark (S21 qualification-only, non-installed)" OFF)
if(LOOP_BUILD_ONLY_CORE_LIBRARY)
    option(LOOP_BUILD_QUICK_CANVAS "Build LoopLibQuick, the direct QQuickItem canvas adapter (P4-S5)" OFF)
else()
    option(LOOP_BUILD_QUICK_CANVAS "Build LoopLibQuick, the direct QQuickItem canvas adapter (P4-S5)" ON)
endif()
option(LOOP_ENABLE_SANITIZERS "Build with ASan+UBSan" OFF)
option(LOOP_BUILD_FUZZERS "Build libFuzzer harnesses" OFF)
if(WIN32)
    set(_LOOP_ENABLE_SENTRY_DEFAULT ON)
else()
    set(_LOOP_ENABLE_SENTRY_DEFAULT OFF)
endif()
option(LOOP_ENABLE_SENTRY "Enable Sentry Native crash reporting (Windows-first)" ${_LOOP_ENABLE_SENTRY_DEFAULT})
# Public ingest DSN for berry-studios/loop-pdf (EU). Runtime SENTRY_DSN overrides;
# SENTRY_DSN=off disables telemetry. CI jobs set SENTRY_DSN=off so builds do not
# flood the project. Confidential workflows should also set SENTRY_DSN=off.
set(_LOOP_SENTRY_DSN_DEFAULT "https://44f65b76ce476befdfb321ca361921fe@o4511731410337792.ingest.de.sentry.io/4511866328449104")
set(LOOP_SENTRY_DSN "${_LOOP_SENTRY_DSN_DEFAULT}" CACHE STRING "Compile-time Sentry DSN (set SENTRY_DSN=off at runtime to disable)")
if(LOOP_SENTRY_DSN STREQUAL "")
    set(LOOP_SENTRY_DSN "${_LOOP_SENTRY_DSN_DEFAULT}" CACHE STRING "Compile-time Sentry DSN (set SENTRY_DSN=off at runtime to disable)" FORCE)
endif()

if(LOOP_ENABLE_SENTRY)
    find_package(Threads REQUIRED)
    find_package(sentry CONFIG REQUIRED)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/LoopSentry.cmake)
endif()

# Loop release profile: ship the single Quick desktop/CLI product surface.
# Developer comparison tools stay source-preserved but default out of release.
option(LOOP_LOOP_DISTRIBUTION "Loop release profile" OFF)

# Compatibility Widgets executables are build-optional and never installed.
# LoopEditor is the only interactive application in either packaging profile.
set(LOOP_LOOP_COMPAT_APPLICATIONS "")

if(LOOP_LOOP_DISTRIBUTION)
    # Release profile builds the installed Quick product only; Widgets-bound
    # developer tools stay opt-in so configure never requires Qt6::Widgets.
    set(_LOOP_BUILD_CODE_GENERATOR_DEFAULT OFF)
    set(_LOOP_BUILD_JBIG2_VIEWER_DEFAULT OFF)
    set(_LOOP_BUILD_EXAMPLE_GENERATOR_DEFAULT OFF)
else()
    set(_LOOP_BUILD_CODE_GENERATOR_DEFAULT ON)
    set(_LOOP_BUILD_JBIG2_VIEWER_DEFAULT ON)
    set(_LOOP_BUILD_EXAMPLE_GENERATOR_DEFAULT ON)
endif()

option(LOOP_BUILD_CODE_GENERATOR "Build CodeGenerator developer tool" ${_LOOP_BUILD_CODE_GENERATOR_DEFAULT})
option(LOOP_BUILD_JBIG2_VIEWER "Build JBIG2_Viewer developer tool" ${_LOOP_BUILD_JBIG2_VIEWER_DEFAULT})
option(LOOP_BUILD_EXAMPLE_GENERATOR "Build PdfExampleGenerator developer tool" ${_LOOP_BUILD_EXAMPLE_GENERATOR_DEFAULT})
if(LOOP_ENABLE_SANITIZERS OR LOOP_BUILD_FUZZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(LOOP_QT_ROOT "" CACHE PATH "Qt root directory")

if(VCPKG_TOOLCHAIN AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/install CACHE PATH "Installation directory" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

# Qt Quick can use Vulkan when the SDK headers are available.  Keep that
# optional discovery quiet in environments that qualify D3D11/software only;
# Qt's GUI package otherwise emits a warning while probing its wrapper.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    set(CMAKE_DISABLE_FIND_PACKAGE_WrapVulkanHeaders TRUE)
endif()

find_package(Qt6 6.11.1 REQUIRED COMPONENTS Core LinguistTools)

set(_LOOP_REQUIRES_WIDGETS OFF)
if(LOOP_BUILD_CODE_GENERATOR OR LOOP_BUILD_JBIG2_VIEWER OR LOOP_BUILD_EXAMPLE_GENERATOR OR LOOP_BUILD_CANVAS_BENCHMARK)
    set(_LOOP_REQUIRES_WIDGETS ON)
endif()

if(LOOP_BUILD_ONLY_CORE_LIBRARY)
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Core Gui Svg Xml Sql)
elseif(_LOOP_REQUIRES_WIDGETS)
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Core Gui Widgets Svg Xml Sql PrintSupport TextToSpeech Concurrent)
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Qml Quick QuickControls2 QuickDialogs2)
else()
    # Qt6::PrintSupport is Widgets-bound in the Qt desktop distribution.  No
    # maintained target in the Widgets-free profile links it, so requesting it
    # here would make a genuinely Widgets-absent Qt prefix fail at configure.
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Core Gui Svg Xml Sql TextToSpeech Concurrent)
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Qml Quick QuickControls2 QuickDialogs2)
endif()
set(LOOP_CONFIGURE_REQUIRES_WIDGETS "${_LOOP_REQUIRES_WIDGETS}" CACHE INTERNAL "Whether configure required Qt6::Widgets")

if(LOOP_BUILD_TESTS)
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Test)
endif()

if(LOOP_BUILD_QUICK_SHELL_SMOKE OR LOOP_BUILD_CANVAS_BENCHMARK)
    if(LOOP_BUILD_ONLY_CORE_LIBRARY)
        message(FATAL_ERROR "Qt Quick qualification targets require a GUI build")
    endif()
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Qml Quick QuickControls2)
endif()

# LoopLibQuick needs Qml and Quick, and deliberately not QuickControls2: it
# contributes a canvas item, not a control, and not linking Controls is what
# keeps a control from being reached from inside the canvas.
if(LOOP_BUILD_QUICK_CANVAS)
    if(LOOP_BUILD_ONLY_CORE_LIBRARY)
        message(FATAL_ERROR "LoopLibQuick requires a GUI build")
    endif()
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS Qml Quick)
elseif(LOOP_BUILD_PRODUCT_QUICK_A11Y_SMOKE)
    message(FATAL_ERROR "The product Quick accessibility smoke requires LOOP_BUILD_QUICK_CANVAS=ON")
endif()

if(LOOP_BUILD_CANVAS_BENCHMARK)
    find_package(Qt6 6.11.1 REQUIRED COMPONENTS QuickWidgets)
endif()

if(NOT LOOP_BUILD_ONLY_CORE_LIBRARY)
    # QTP0004 is provided by Qt Qml.  The fuzz configuration intentionally
    # builds only LoopLibCore and therefore does not discover that module.
    qt_policy(SET QTP0004 NEW)
endif()
set(QT_NO_MISSING_CATALOG_LANGUAGE_WARNING ON)
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
    if(LOOP_ENABLE_SENTRY)
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
    set(LOOP_LINUX ON)
endif()

# Parallel STL (libstdc++/libc++) on Linux needs TBB for both GCC and Clang.
if(LOOP_LINUX)
    find_package(TBB CONFIG REQUIRED)
    set(LOOP_USE_TBB ON)
endif()

if(LOOP_ENABLE_SENTRY)
    find_package(sentry CONFIG REQUIRED)
endif()

option(LOOP_INSTALL_TO_USR "Install to usr directory" ${VCPKG_TOOLCHAIN})

if(LOOP_LINUX)
    set(LOOP_INSTALL_LIB_DIR_IMPL ${CMAKE_INSTALL_LIBDIR})
    set(LOOP_INSTALL_BIN_DIR_IMPL ${CMAKE_INSTALL_BINDIR})
else()
    set(LOOP_INSTALL_LIB_DIR_IMPL ${CMAKE_INSTALL_BINDIR})
    set(LOOP_INSTALL_BIN_DIR_IMPL ${CMAKE_INSTALL_BINDIR})
endif()

if(LOOP_INSTALL_TO_USR)
    set(LOOP_INSTALL_LIB_DIR usr/${LOOP_INSTALL_LIB_DIR_IMPL})
    set(LOOP_INSTALL_BIN_DIR usr/${LOOP_INSTALL_BIN_DIR_IMPL})
    set(LOOP_INSTALL_SHARE_DIR usr/${CMAKE_INSTALL_DATAROOTDIR})
    set(LOOP_INSTALL_INCLUDE_DIR usr/${CMAKE_INSTALL_INCLUDEDIR})
else()
    set(LOOP_INSTALL_LIB_DIR ${LOOP_INSTALL_LIB_DIR_IMPL})
    set(LOOP_INSTALL_BIN_DIR ${LOOP_INSTALL_BIN_DIR_IMPL})
    set(LOOP_INSTALL_SHARE_DIR ${CMAKE_INSTALL_DATAROOTDIR})
    set(LOOP_INSTALL_INCLUDE_DIR ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(LOOP_LINUX)
    set(LOOP_PLUGINS_DIR ${LOOP_INSTALL_LIB_DIR}/loop)
    set(LOOP_INSTALL_TRANS_DIR ${LOOP_INSTALL_SHARE_DIR}/loop/translations)
else()
    set(LOOP_PLUGINS_DIR ${LOOP_INSTALL_LIB_DIR}/pdfplugins)
    set(LOOP_INSTALL_TRANS_DIR ${LOOP_INSTALL_BIN_DIR}/translations)
endif()

# Relative paths are used by the config
cmake_path(RELATIVE_PATH LOOP_PLUGINS_DIR
           BASE_DIRECTORY "${LOOP_INSTALL_BIN_DIR}"
           OUTPUT_VARIABLE LOOP_PLUGINS_RELATIVE_PATH)
cmake_path(RELATIVE_PATH LOOP_INSTALL_TRANS_DIR
           BASE_DIRECTORY "${LOOP_INSTALL_BIN_DIR}"
           OUTPUT_VARIABLE LOOP_TRANSLATIONS_RELATIVE_PATH)
configure_file(config.h.cmake config.h)

add_subdirectory(LoopLibCore)

if(LOOP_INSTALL_INCLUDE)
    # Install public headers from LoopLibCore/include
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/LoopLibCore/sources/
            DESTINATION ${LOOP_INSTALL_INCLUDE_DIR}/LoopLibCore
            FILES_MATCHING PATTERN "*.h")

    # Install generated export header
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/looplibcore_export.h
            DESTINATION ${LOOP_INSTALL_INCLUDE_DIR}/LoopLibCore)

endif()

if(NOT LOOP_BUILD_ONLY_CORE_LIBRARY)
    # Host-neutral interaction seam. Depends only on LoopLibCore and is not
    # installed; see LoopLibInteraction/CMakeLists.txt.
    add_subdirectory(LoopLibInteraction)

    # The Quick product host and its direct QQuickItem canvas are one build
    # boundary. An OFF build remains a supported tools/legacy-host build and
    # must not descend into a target that requires LoopLibQuick.
    if(LOOP_BUILD_QUICK_CANVAS)
        add_subdirectory(LoopLibQuick)
    endif()

    if(LOOP_BUILD_CODE_GENERATOR)
        add_subdirectory(CodeGenerator)
    endif()
    if(LOOP_BUILD_JBIG2_VIEWER)
        add_subdirectory(JBIG2_Viewer)
    endif()
    if(LOOP_BUILD_EXAMPLE_GENERATOR)
        add_subdirectory(PdfExampleGenerator)
    endif()
    add_subdirectory(PdfTool)
    add_subdirectory(loop-preflight/tools)
    add_subdirectory(loop-ocr/tools)
    if(LOOP_BUILD_QUICK_SHELL_SMOKE)
        add_subdirectory(QuickShellSmoke)
    endif()
    if(LOOP_BUILD_CANVAS_BENCHMARK)
        add_subdirectory(CanvasBenchmark)
    endif()
    if(LOOP_BUILD_QUICK_CANVAS)
        add_subdirectory(LoopEditor)
        if(LOOP_BUILD_PRODUCT_QUICK_A11Y_SMOKE)
            add_subdirectory(ProductQuickAccessibilitySmoke)
        endif()
    endif()
    add_subdirectory(WixInstaller)
endif()

if(LOOP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(UnitTests)
endif()

if(LOOP_BUILD_FUZZERS)
    add_subdirectory(Fuzz)
endif()

qt_collect_translation_source_targets(i18n_targets)
message("CMAKE_TRANSLATION_TARGETS = " ${i18n_targets})

set(LOOP_TRANSLATION_TS_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/translations)
set(LOOP_TRANSLATION_QM_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOOP_INSTALL_TRANS_DIR})

message("LOOP_TRANSLATION_TS_OUTPUT_DIRECTORY" ${LOOP_TRANSLATION_TS_OUTPUT_DIRECTORY})
message("LOOP_TRANSLATION_QM_OUTPUT_DIRECTORY" ${LOOP_TRANSLATION_QM_OUTPUT_DIRECTORY})

# Only add translation targets for targets that actually exist in this build.
foreach(_tgt LoopEditor PdfTool)
    if(TARGET ${_tgt})
        list(APPEND _existing_translation_targets ${_tgt})
    endif()
endforeach()
if(_existing_translation_targets)
    qt_add_translations(TARGETS ${_existing_translation_targets}
                        SOURCE_TARGETS ${i18n_targets}
                        MERGE_QT_TRANSLATIONS
                        LUPDATE_OPTIONS -no-obsolete
                        TS_OUTPUT_DIRECTORY ${LOOP_TRANSLATION_TS_OUTPUT_DIRECTORY}
                        QM_OUTPUT_DIRECTORY ${LOOP_TRANSLATION_QM_OUTPUT_DIRECTORY}
                        QM_FILES_OUTPUT_VARIABLE LOOP_QM_FILES)
endif()

if(WIN32)
    set(LOOP_LUPDATE_TARGET "${PROJECT_NAME}_lupdate")
    if(TARGET ${LOOP_LUPDATE_TARGET})
        add_custom_command(TARGET ${LOOP_LUPDATE_TARGET}
                           POST_BUILD
                           COMMAND ${CMAKE_COMMAND}
                                   -DTS_OUTPUT_DIRECTORY=${LOOP_TRANSLATION_TS_OUTPUT_DIRECTORY}
                                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NormalizeTsLineEndings.cmake
                           COMMENT "Normalizing TS files to CRLF")
    endif()
endif()

message("CMAKE_PREFIX_PATH = " ${CMAKE_PREFIX_PATH})
message("CMAKE_TOOLCHAIN_FILE = " ${CMAKE_TOOLCHAIN_FILE})

configure_file(version.txt.in version.txt)

if(NOT LOOP_BUILD_ONLY_CORE_LIBRARY)
    install(FILES ${LOOP_QM_FILES}
            DESTINATION ${LOOP_INSTALL_TRANS_DIR})

    if(WIN32)
        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Desktop/44x44/
                RUNTIME DESTINATION ${LOOP_INSTALL_BIN_DIR}/assets/44x44
                FILES_MATCHING
                PATTERN *.png)
        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Desktop/150x150/
                RUNTIME DESTINATION ${LOOP_INSTALL_BIN_DIR}/assets/150x150
                FILES_MATCHING
                PATTERN *.png)

        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/AppxManifest.xml.in ${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/AppxManifest.xml DESTINATION ${LOOP_INSTALL_BIN_DIR})
        install(FILES assets/logo.png DESTINATION ${LOOP_INSTALL_BIN_DIR}/assets)
    else()
        configure_file(make-package.sh.in make-package.sh)

        if(LOOP_LOOP_DISTRIBUTION)
            set(_LOOP_DESKTOP_ENTRIES
                Desktop/io.github.mberrys.Loop-pdf.desktop)
            set(_LOOP_DESKTOP_ICONS
                Desktop/io.github.mberrys.Loop-pdf.svg)
            set(_LOOP_DESKTOP_PNGS
                Desktop/128x128/io.github.mberrys.Loop-pdf.png)
        else()
            set(_LOOP_DESKTOP_ENTRIES
                Desktop/io.github.mberrys.Loop-pdf.LoopEditor.desktop
                Desktop/io.github.mberrys.Loop-pdf.desktop)
            set(_LOOP_DESKTOP_ICONS
                Desktop/io.github.mberrys.Loop-pdf.LoopEditor.svg
                Desktop/io.github.mberrys.Loop-pdf.svg)
            set(_LOOP_DESKTOP_PNGS
                Desktop/128x128/io.github.mberrys.Loop-pdf.LoopEditor.png
                Desktop/128x128/io.github.mberrys.Loop-pdf.png)
        endif()

        install(FILES ${_LOOP_DESKTOP_ENTRIES}
                      DESTINATION ${LOOP_INSTALL_SHARE_DIR}/applications)
        install(FILES ${_LOOP_DESKTOP_ICONS}
                      DESTINATION ${LOOP_INSTALL_SHARE_DIR}/icons/hicolor/scalable/apps)
        install(FILES Desktop/io.github.mberrys.Loop-pdf.appdata.xml DESTINATION ${LOOP_INSTALL_SHARE_DIR}/metainfo)
        install(FILES ${_LOOP_DESKTOP_PNGS}
                      DESTINATION ${LOOP_INSTALL_SHARE_DIR}/icons/hicolor/128x128/apps)
    endif()

endif()

if(LOOP_INSTALL_DEPENDENCIES AND NOT LOOP_BUILD_ONLY_CORE_LIBRARY)
    install(DIRECTORY ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/
        RUNTIME DESTINATION ${LOOP_INSTALL_LIB_DIR}/
        FILES_MATCHING
        PATTERN "*.dll"
        PATTERN "*.so"
        PATTERN "*.dylib"
    )

    if("${LOOP_QT_ROOT}" STREQUAL "")
        message(WARNING "Set variable LOOP_QT_ROOT to Qt6 root directory")
    endif()

    if(LOOP_INSTALL_QT_DEPENDENCIES)
        if(LOOP_LOOP_DISTRIBUTION)
            set(_LOOP_QT_DLL_REGEX "(Qt6Core|Qt6Gui|Qt6Svg|Qt6TextToSpeech|Qt6Xml|Qt6Multimedia|Qt6Network|Qt6Qml|Qt6Quick|Qt6QuickControls2|Qt6QuickDialogs2)\\..*")
        else()
            set(_LOOP_QT_DLL_REGEX "(Qt6Core|Qt6Gui|Qt6PrintSupport|Qt6Svg|Qt6TextToSpeech|Qt6Widgets|Qt6Xml|Qt6Multimedia|Qt6Network|Qt6Qml|Qt6Quick|Qt6QuickControls2|Qt6QuickDialogs2)\\..*")
        endif()
        install(DIRECTORY ${LOOP_QT_ROOT}/bin/
            RUNTIME DESTINATION ${LOOP_INSTALL_LIB_DIR}/
            FILES_MATCHING
            REGEX "${_LOOP_QT_DLL_REGEX}"
            PATTERN "Debug" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
        )

        # ICU is a load-time dependency of Qt6Core on Windows aqt builds; without
        # these DLLs beside PdfTool.exe the MSI-installed binary fails with
        # STATUS_DLL_NOT_FOUND (0xC0000135) even when Qt6*.dll themselves are present.
        install(DIRECTORY ${LOOP_QT_ROOT}/bin/
            RUNTIME DESTINATION ${LOOP_INSTALL_LIB_DIR}/
            FILES_MATCHING
            PATTERN "icu*.dll"
            PATTERN "Debug" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
        )

        install(DIRECTORY ${LOOP_QT_ROOT}/plugins/platforms/
            RUNTIME DESTINATION ${LOOP_INSTALL_LIB_DIR}/platforms/
            FILES_MATCHING
            PATTERN "qwindows.dll"
            PATTERN "qoffscreen.dll"
            PATTERN "*.so"
            PATTERN "*.dylib"
            PATTERN "*.pdb" EXCLUDE
        )

        install(DIRECTORY ${LOOP_QT_ROOT}/plugins/iconengines/
            RUNTIME DESTINATION ${LOOP_INSTALL_LIB_DIR}/iconengines/
            FILES_MATCHING
            REGEX "qsvgicon\\..*"
            PATTERN "*.pdb" EXCLUDE
        )

        install(DIRECTORY ${LOOP_QT_ROOT}/plugins/imageformats/
            RUNTIME DESTINATION ${LOOP_INSTALL_LIB_DIR}/imageformats/
            FILES_MATCHING
            PATTERN "*.dll"
            PATTERN "*.so"
            PATTERN "*.dylib"
            REGEX "d\\..*" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
        )

        if(NOT LOOP_LOOP_DISTRIBUTION)
            install(DIRECTORY ${LOOP_QT_ROOT}/plugins/styles/
                RUNTIME DESTINATION ${LOOP_INSTALL_LIB_DIR}/styles/
                FILES_MATCHING
                PATTERN "*.dll"
                PATTERN "*.so"
                PATTERN "*.dylib"
                REGEX "d\\..*" EXCLUDE
                PATTERN "*.pdb" EXCLUDE
            )
        endif()

        install(DIRECTORY ${LOOP_QT_ROOT}/plugins/texttospeech/
            RUNTIME DESTINATION ${LOOP_INSTALL_LIB_DIR}/texttospeech/
            FILES_MATCHING
            PATTERN "*.dll"
            PATTERN "*.so"
            PATTERN "*.dylib"
            REGEX "d\\..*" EXCLUDE
            PATTERN "*.pdb" EXCLUDE
        )

        install(DIRECTORY ${LOOP_QT_ROOT}/qml/
            DESTINATION ${LOOP_INSTALL_LIB_DIR}/qml/
            PATTERN "*.pdb" EXCLUDE
        )
    endif()

    if(WIN32 AND MSVC AND LOOP_INSTALL_MSVC_REDISTRIBUTABLE)
        if(DEFINED ENV{MSVC_REDISTRIBUTABLES_PATH})
            set(MSVC_REDISTRIBUTABLES_PATH $ENV{MSVC_REDISTRIBUTABLES_PATH})
            message(STATUS "Using existing MSVC Redistributable Package Path = ${MSVC_REDISTRIBUTABLES_PATH}")
        else()
            get_filename_component(MSVC_REDISTRIBUTABLES_PATH $ENV{VCToolsRedistDir}/$ENV{VSCMD_ARG_TGT_ARCH}/Microsoft.VC${MSVC_TOOLSET_VERSION}.CRT/ ABSOLUTE)
            message(STATUS "MSVC Redistributable Package Path = ${MSVC_REDISTRIBUTABLES_PATH}")
        endif()

        install(DIRECTORY ${MSVC_REDISTRIBUTABLES_PATH}/
            RUNTIME DESTINATION ${LOOP_INSTALL_LIB_DIR}/
            FILES_MATCHING
            PATTERN "*.dll"
        )
    endif()
endif()

if(LOOP_ENABLE_SENTRY AND TARGET sentry_crashpad::crashpad_handler)
    get_target_property(_loop_sentry_crashpad_handler_install sentry_crashpad::crashpad_handler IMPORTED_LOCATION_RELEASE)
    if(NOT _loop_sentry_crashpad_handler_install)
        get_target_property(_loop_sentry_crashpad_handler_install sentry_crashpad::crashpad_handler IMPORTED_LOCATION)
    endif()

    if(_loop_sentry_crashpad_handler_install)
        if(WIN32)
            install(PROGRAMS ${_loop_sentry_crashpad_handler_install}
                    DESTINATION ${LOOP_INSTALL_BIN_DIR}
                    RENAME crashpad_handler.exe)
        else()
            install(PROGRAMS ${_loop_sentry_crashpad_handler_install}
                    DESTINATION ${LOOP_INSTALL_BIN_DIR}
                    RENAME crashpad_handler)
        endif()
    endif()

    if(TARGET sentry_crashpad::crashpad_wer)
        get_target_property(_loop_sentry_crashpad_wer_install sentry_crashpad::crashpad_wer IMPORTED_LOCATION_RELEASE)
        if(NOT _loop_sentry_crashpad_wer_install)
            get_target_property(_loop_sentry_crashpad_wer_install sentry_crashpad::crashpad_wer IMPORTED_LOCATION)
        endif()

        if(_loop_sentry_crashpad_wer_install)
            install(FILES ${_loop_sentry_crashpad_wer_install}
                    DESTINATION ${LOOP_INSTALL_BIN_DIR})
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
