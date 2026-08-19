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

set(SCANNER_PLUGIN_SOURCES
    scandialog.cpp
    scannerbackend.cpp
    scannedpdfbuilder.cpp
    scannerplugin.cpp
    icons.qrc
)

if(WIN32)
    list(APPEND SCANNER_PLUGIN_SOURCES wiascannerbackend.cpp)
endif()

if(LOUPE_LINUX)
    find_library(SANE_LIBRARY sane)
    find_path(SANE_INCLUDE_DIR sane/sane.h)

    if(SANE_LIBRARY AND SANE_INCLUDE_DIR)
        list(APPEND SCANNER_PLUGIN_SOURCES sanescannerbackend.cpp)
        set(LOUPE_HAS_SANE ON)
    endif()
endif()

add_library(ScannerPlugin SHARED ${SCANNER_PLUGIN_SOURCES})

target_link_libraries(ScannerPlugin PRIVATE LoupeLibCore LoupeLibWidgets Qt6::Core Qt6::Gui Qt6::Widgets)

if(LOUPE_HAS_SANE)
    target_compile_definitions(ScannerPlugin PRIVATE LOUPE_HAS_SANE)
    target_include_directories(ScannerPlugin PRIVATE ${SANE_INCLUDE_DIR})
    target_link_libraries(ScannerPlugin PRIVATE ${SANE_LIBRARY})
endif()

set_target_properties(ScannerPlugin PROPERTIES
                      VERSION ${LOUPE_VERSION}
                      SOVERSION ${LOUPE_VERSION}
                      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_PLUGINS_DIR}
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_PLUGINS_DIR})

install(TARGETS ScannerPlugin RUNTIME DESTINATION ${LOUPE_PLUGINS_DIR} LIBRARY DESTINATION ${LOUPE_PLUGINS_DIR})
