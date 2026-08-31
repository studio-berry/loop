qt_policy(SET QTP0001 NEW)

qt_add_executable(QuickShellSmoke
    main.cpp
)

qt_add_qml_module(QuickShellSmoke
    URI Loop.QuickShellSmoke
    VERSION 1.0
    QML_FILES
        QuickShellSmoke.qml
)

target_link_libraries(QuickShellSmoke
    PRIVATE
        LoopLibCore
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
)
