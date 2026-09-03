qt_policy(SET QTP0001 NEW)

qt_add_executable(ProductQuickAccessibilitySmoke
    main.cpp
    ../LoopEditor/editorhost.cpp
    ../LoopEditor/editorhost.h
    ../LoopEditor/focusrestoration.cpp
    ../LoopEditor/focusrestoration.h
    ../LoopEditor/quickdocumentmodel.cpp
    ../LoopEditor/quickdocumentmodel.h
    ../LoopEditor/documentviewsession.cpp
    ../LoopEditor/documentviewsession.h
)

set_source_files_properties(
    ../LoopEditor/qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml
)
set_source_files_properties(
    ../LoopEditor/qml/Workspace.qml PROPERTIES QT_RESOURCE_ALIAS Workspace.qml
)
set_source_files_properties(
    ../LoopEditor/qml/PreflightPane.qml PROPERTIES QT_RESOURCE_ALIAS PreflightPane.qml
)
set_source_files_properties(
    ../LoopEditor/qml/InspectorPane.qml PROPERTIES QT_RESOURCE_ALIAS InspectorPane.qml
)
set_source_files_properties(
    ../LoopEditor/qml/DocumentPane.qml PROPERTIES QT_RESOURCE_ALIAS DocumentPane.qml
)
set_source_files_properties(
    ../LoopEditor/qml/CanvasPane.qml PROPERTIES QT_RESOURCE_ALIAS CanvasPane.qml
)

qt_add_qml_module(ProductQuickAccessibilitySmoke
    URI Loop.Quick
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/Loop/Quick"
    NO_GENERATE_EXTRA_QMLDIRS
    QML_FILES
        ../LoopEditor/qml/Main.qml
        ../LoopEditor/qml/Workspace.qml
        ../LoopEditor/qml/PreflightPane.qml
        ../LoopEditor/qml/InspectorPane.qml
        ../LoopEditor/qml/DocumentPane.qml
        ../LoopEditor/qml/CanvasPane.qml
)

target_link_libraries(ProductQuickAccessibilitySmoke
    PRIVATE
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

target_include_directories(ProductQuickAccessibilitySmoke PRIVATE
    ${CMAKE_SOURCE_DIR}/LoopEditor
)

qt_import_qml_plugins(ProductQuickAccessibilitySmoke)

set_target_properties(ProductQuickAccessibilitySmoke PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOOP_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOOP_INSTALL_BIN_DIR}
)
