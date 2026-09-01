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

option(LOOP_BUILD_OCR_SERVICE "Build LoopOcrService via PyInstaller (requires Python venv)" OFF)
option(LOOP_BUNDLE_OCR_SERVICE "Install prebuilt LoopOcrService from loop-ocr/prebuilt" OFF)

set(LOOP_OCR_PREBUILT_DIR ${CMAKE_SOURCE_DIR}/loop-ocr/prebuilt/LoopOcrService)

if(LOOP_BUILD_OCR_SERVICE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_target(LoopOcrServiceBundle
        COMMAND ${Python3_EXECUTABLE} -m PyInstaller
            ${CMAKE_SOURCE_DIR}/loop-ocr/tools/pyinstaller.spec
            --distpath ${CMAKE_SOURCE_DIR}/loop-ocr/prebuilt
            --workpath ${CMAKE_BINARY_DIR}/loop-ocr/pyinstaller
            --noconfirm
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/loop-ocr/tools
        COMMENT "Building LoopOcrService PyInstaller bundle"
    )
endif()

if(LOOP_BUNDLE_OCR_SERVICE)
    if(NOT EXISTS "${LOOP_OCR_PREBUILT_DIR}/LoopOcrService.exe" AND NOT EXISTS "${LOOP_OCR_PREBUILT_DIR}/LoopOcrService")
        message(WARNING "LOOP_BUNDLE_OCR_SERVICE is ON but prebuilt LoopOcrService was not found at ${LOOP_OCR_PREBUILT_DIR}")
    else()
        install(DIRECTORY ${LOOP_OCR_PREBUILT_DIR}/
                DESTINATION ${LOOP_INSTALL_BIN_DIR}/LoopOcrService
                USE_SOURCE_PERMISSIONS)
        install(FILES ${CMAKE_SOURCE_DIR}/loop-ocr/THIRD_PARTY_NOTICES.txt
                DESTINATION ${LOOP_INSTALL_BIN_DIR}/LoopOcrService)
    endif()
endif()

install(DIRECTORY ${CMAKE_SOURCE_DIR}/loop-ocr/schemas/
        DESTINATION ${LOOP_INSTALL_SHARE_DIR}/loop/ocr/schemas)
