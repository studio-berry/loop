qt_policy(SET QTP0001 NEW)

qt_add_executable(ProductQuickAccessibilitySmoke
    main.cpp
)

target_link_libraries(ProductQuickAccessibilitySmoke
    PRIVATE
        LoopEditorQuick
        LoopEditorQuickplugin
        LoopEditorQuickplugin_init
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
