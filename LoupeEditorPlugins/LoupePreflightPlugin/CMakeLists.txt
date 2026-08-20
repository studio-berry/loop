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

set(LOUPE_PREFLIGHT_PLUGIN_SOURCES
    loupepreflightplugin.cpp
    preflightreportdockwidget.cpp
    preflightreportmodel.cpp
    repairpreviewdialog.cpp
    repairpreviewdialog.h
    icons.qrc
)

add_library(LoupePreflightPlugin SHARED ${LOUPE_PREFLIGHT_PLUGIN_SOURCES})

target_link_libraries(LoupePreflightPlugin PRIVATE LoupeLibCore LoupeLibWidgets Qt6::Core Qt6::Gui Qt6::Widgets)

set(LOUPE_PREFLIGHT_PROFILES_DIR ${LOUPE_INSTALL_SHARE_DIR}/loupe/profiles)
cmake_path(RELATIVE_PATH LOUPE_PREFLIGHT_PROFILES_DIR
           BASE_DIRECTORY "${LOUPE_INSTALL_BIN_DIR}"
           OUTPUT_VARIABLE LOUPE_PREFLIGHT_PROFILES_RELATIVE_PATH)

target_compile_definitions(LoupePreflightPlugin PRIVATE
    LOUPE_PREFLIGHT_PROFILES_RELATIVE_PATH="${LOUPE_PREFLIGHT_PROFILES_RELATIVE_PATH}"
    LOUPE_PREFLIGHT_SCHEMA_VERSION=3)

set_target_properties(LoupePreflightPlugin PROPERTIES
                      VERSION ${LOUPE_VERSION}
                      SOVERSION ${LOUPE_VERSION}
                      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_PLUGINS_DIR}
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_PLUGINS_DIR})

install(TARGETS LoupePreflightPlugin RUNTIME DESTINATION ${LOUPE_PLUGINS_DIR} LIBRARY DESTINATION ${LOUPE_PLUGINS_DIR})
install(FILES ${CMAKE_SOURCE_DIR}/loupe-preflight/profiles/loupe-default.json
        DESTINATION ${LOUPE_PREFLIGHT_PROFILES_DIR})
install(DIRECTORY ${CMAKE_SOURCE_DIR}/loupe-preflight/schemas/
        DESTINATION ${LOUPE_PREFLIGHT_PROFILES_DIR}/schemas)
