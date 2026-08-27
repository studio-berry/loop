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

add_executable(LoupePageMaster
    aboutdialog.cpp
    aboutdialog.h
    assembleoutputsettingsdialog.cpp
    assembleoutputsettingsdialog.h
    croppagesdialog.cpp
    croppagesdialog.h
    imageoptimizationsettingsdialog.cpp
    imageoptimizationsettingsdialog.h
    itempropertiesdialog.cpp
    itempropertiesdialog.h
    main.cpp
    mainwindow.cpp
    mainwindow.h
    pageitemdelegate.cpp
    pageitemdelegate.h
    pdfpageimportdialog.cpp
    pdfpageimportdialog.h
    pageitempreviewrenderer.cpp
    pageitempreviewrenderer.h
    pageitemmodel.cpp
    pageitemmodel.h
    selectoutlinetoregroupdialog.cpp
    selectoutlinetoregroupdialog.h
    resources.qrc
    icon.rc
    app.qrc
)

target_link_libraries(LoupePageMaster PRIVATE LoupeLibCore LoupeLibWidgets Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent)

set_target_properties(LoupePageMaster PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

if(LOUPE_ENABLE_SENTRY)
    loupe_deploy_sentry_crashpad(LoupePageMaster)
endif()
