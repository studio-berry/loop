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

add_library(LoopEditorQuick STATIC
    documentviewsession.cpp
    documentviewsession.h
    editorhost.cpp
    editorhost.h
    focusrestoration.cpp
    focusrestoration.h
)

qt_add_qml_module(LoopEditorQuick
    URI Loop.Quick
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Loop/Quick"
    QML_FILES
        qml/Main.qml
        qml/Workspace.qml
        qml/CanvasPane.qml
        qml/PreflightPane.qml
        qml/InspectorPane.qml
)

target_link_libraries(LoopEditorQuick
    PUBLIC
        LoopLibQuick
        LoopLibInteraction
        LoopLibCore
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
)

set_target_properties(LoopEditorQuick PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(LoopEditorQuick PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

qt_add_executable(LoopEditor
    main.cpp
    icon.rc
    app.qrc
)

target_link_libraries(LoopEditor
    PRIVATE
        LoopEditorQuick
        LoopEditorQuickplugin
        LoopEditorQuickplugin_init
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickDialogs2
)

qt_import_qml_plugins(LoopEditor)

set_target_properties(LoopEditor PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOOP_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOOP_INSTALL_BIN_DIR}
)

install(TARGETS LoopEditor
    RUNTIME DESTINATION ${LOOP_INSTALL_BIN_DIR}
    LIBRARY DESTINATION ${LOOP_INSTALL_LIB_DIR}
    ARCHIVE DESTINATION ${LOOP_INSTALL_LIB_DIR}
)

if(LOOP_ENABLE_SENTRY)
    loop_deploy_sentry_crashpad(LoopEditor)
endif()
