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

qt_policy(SET QTP0001 NEW)

add_library(LoupeEditorQuick STATIC
    editorhost.cpp
    editorhost.h
    focusrestoration.cpp
    focusrestoration.h
)

qt_add_qml_module(LoupeEditorQuick
    URI Loupe.Quick
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Workspace.qml
        qml/CanvasPane.qml
        qml/PreflightPane.qml
        qml/InspectorPane.qml
)

target_link_libraries(LoupeEditorQuick
    PUBLIC
        LoupeLibQuick
        LoupeLibInteraction
        LoupeLibCore
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
)

set_target_properties(LoupeEditorQuick PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(LoupeEditorQuick PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

qt_add_executable(LoupeEditor
    main.cpp
    icon.rc
    app.qrc
)

target_link_libraries(LoupeEditor
    PRIVATE
        LoupeEditorQuick
        LoupeEditorQuickplugin
        LoupeEditorQuickplugin_init
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
)

qt_import_qml_plugins(LoupeEditor)

set_target_properties(LoupeEditor PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

install(TARGETS LoupeEditor LoupeEditorQuick
    RUNTIME DESTINATION ${LOUPE_INSTALL_BIN_DIR}
    LIBRARY DESTINATION ${LOUPE_INSTALL_LIB_DIR}
    ARCHIVE DESTINATION ${LOUPE_INSTALL_LIB_DIR}
)

if(LOUPE_ENABLE_SENTRY)
    loupe_deploy_sentry_crashpad(LoupeEditor)
endif()

option(LOUPE_BUILD_EDITOR_WIDGETS_ORACLE "Build the non-installed Widgets editor oracle" ON)

if(LOUPE_BUILD_EDITOR_WIDGETS_ORACLE)
    add_executable(LoupeEditorWidgetsOracle
        widgets_oracle_main.cpp
        icon.rc
        app.qrc
    )

    target_link_libraries(LoupeEditorWidgetsOracle
        PRIVATE
            LoupeLibCore
            LoupeLibWidgets
            LoupeLibGui
            Qt6::Core
            Qt6::Gui
            Qt6::Widgets
    )

    set_target_properties(LoupeEditorWidgetsOracle PROPERTIES
        WIN32_EXECUTABLE ON
        MACOSX_BUNDLE ON
        EXCLUDE_FROM_ALL OFF
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
    )
endif()
