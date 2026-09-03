qt_policy(SET QTP0001 NEW)

qt_add_executable(ProductQuickAccessibilitySmoke
    main.cpp
)

qt_add_qml_module(ProductQuickAccessibilitySmoke
    URI Loop.Quick
    VERSION 1.0
    QML_FILES
        ${CMAKE_SOURCE_DIR}/LoopEditor/qml/Main.qml
        ${CMAKE_SOURCE_DIR}/LoopEditor/qml/Workspace.qml
        ${CMAKE_SOURCE_DIR}/LoopEditor/qml/PreflightPane.qml
        ${CMAKE_SOURCE_DIR}/LoopEditor/qml/InspectorPane.qml
        ${CMAKE_SOURCE_DIR}/LoopEditor/qml/DocumentPane.qml
        ${CMAKE_SOURCE_DIR}/LoopEditor/qml/CanvasPane.qml
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

qt_import_qml_plugins(ProductQuickAccessibilitySmoke)

set_target_properties(ProductQuickAccessibilitySmoke PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOOP_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOOP_INSTALL_BIN_DIR}
)
