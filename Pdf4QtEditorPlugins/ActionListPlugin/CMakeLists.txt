# MIT License
#
# Copyright (c) 2018-2025 Jakub Melka and Contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software.

add_library(ActionListPlugin SHARED
    actionlistdialog.cpp
    actionlistdialog.h
    actionlistplugin.cpp
    actionlistplugin.h
)

target_link_libraries(ActionListPlugin PRIVATE
    Pdf4QtLibCore
    Pdf4QtLibWidgets
    Pdf4QtLibGui
    Qt6::Concurrent
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

set_target_properties(ActionListPlugin PROPERTIES
                      VERSION ${PDF4QT_VERSION}
                      SOVERSION ${PDF4QT_VERSION}
                      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${PDF4QT_PLUGINS_DIR}
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${PDF4QT_PLUGINS_DIR})

install(TARGETS ActionListPlugin RUNTIME DESTINATION ${PDF4QT_PLUGINS_DIR} LIBRARY DESTINATION ${PDF4QT_PLUGINS_DIR})
