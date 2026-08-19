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
    LoupeLibCore
    LoupeLibWidgets
    LoupeLibGui
    Qt6::Concurrent
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

set_target_properties(ActionListPlugin PROPERTIES
                      VERSION ${LOUPE_VERSION}
                      SOVERSION ${LOUPE_VERSION}
                      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_PLUGINS_DIR}
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_PLUGINS_DIR})

install(TARGETS ActionListPlugin RUNTIME DESTINATION ${LOUPE_PLUGINS_DIR} LIBRARY DESTINATION ${LOUPE_PLUGINS_DIR})
