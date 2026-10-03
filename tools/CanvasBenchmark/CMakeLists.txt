qt_policy(SET QTP0001 NEW)

qt_add_executable(CanvasBenchmark
    main.cpp
)

qt_add_qml_module(CanvasBenchmark
    URI Loop.CanvasBenchmark
    VERSION 1.0
    QML_FILES
        CanvasBenchmark.qml
)

target_link_libraries(CanvasBenchmark
    PRIVATE
        LoopLibCore
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickWidgets
        Qt6::Widgets
)

set_target_properties(CanvasBenchmark PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOOP_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOOP_INSTALL_BIN_DIR}
)
