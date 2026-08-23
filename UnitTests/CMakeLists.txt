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

add_executable(UnitTests
	tst_lexicalanalyzertest.cpp
)

target_link_libraries(UnitTests PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTests PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTests  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTests")

add_executable(UnitTestsIncrementalSave
    tst_incrementalsavetest.cpp
)

target_link_libraries(UnitTestsIncrementalSave PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsIncrementalSave PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsIncrementalSave "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsIncrementalSave")

add_executable(UnitTestsRecovery
    tst_recoverytest.cpp
)

target_link_libraries(UnitTestsRecovery PRIVATE
    LoupeLibCore
    LoupeLibGui
    LoupeLibWidgets
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
)

target_include_directories(UnitTestsRecovery PRIVATE
    ${CMAKE_SOURCE_DIR}/LoupeLibGui
    ${CMAKE_BINARY_DIR}/${INSTALL_INCLUDEDIR}
)

set_target_properties(UnitTestsRecovery PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsRecovery "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsRecovery")

add_executable(UnitTestsUndoRedoManager
    tst_undoredomanagertest.cpp
)

target_link_libraries(UnitTestsUndoRedoManager PRIVATE
    LoupeLibCore
    LoupeLibGui
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
)

target_include_directories(UnitTestsUndoRedoManager PRIVATE
    ${CMAKE_SOURCE_DIR}/LoupeLibGui
    ${CMAKE_BINARY_DIR}/${INSTALL_INCLUDEDIR}
)

set_target_properties(UnitTestsUndoRedoManager PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsUndoRedoManager "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsUndoRedoManager")

add_executable(UnitTestsProcessingBudget
    tst_processingbudgettest.cpp
)

target_link_libraries(UnitTestsProcessingBudget PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsProcessingBudget PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_executable(UnitTestsAccessibility
    tst_accessibilitytest.cpp
)

target_link_libraries(UnitTestsAccessibility PRIVATE
    LoupeLibWidgets
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
)

set_target_properties(UnitTestsAccessibility PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsProcessingBudget "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsProcessingBudget")
add_test(UnitTestsAccessibility "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsAccessibility")

add_executable(UnitTestsJobScheduler
    tst_jobschedulertest.cpp
)

target_link_libraries(UnitTestsJobScheduler PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsJobScheduler PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsJobScheduler "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsJobScheduler")

add_executable(UnitTestsInteractionTrace
    tst_interactiontracetest.cpp
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources/pdfinteractiontrace_p.cpp
)

target_link_libraries(UnitTestsInteractionTrace PRIVATE Qt6::Core Qt6::Test)
target_include_directories(UnitTestsInteractionTrace PRIVATE
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources)

set_target_properties(UnitTestsInteractionTrace PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsInteractionTrace "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsInteractionTrace")

add_executable(UnitTestsInteractionState
    tst_interactionstatetest.cpp
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources/pdfinteractionstate_p.cpp
)

target_link_libraries(UnitTestsInteractionState PRIVATE
    LoupeLibCore
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
target_include_directories(UnitTestsInteractionState PRIVATE
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources
)

set_target_properties(UnitTestsInteractionState PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsInteractionState "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsInteractionState")

add_executable(UnitTestsInteractionWidget
    tst_interactionwidgettest.cpp
)

target_link_libraries(UnitTestsInteractionWidget PRIVATE
    LoupeLibWidgets
    LoupeLibCore
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
)
target_include_directories(UnitTestsInteractionWidget PRIVATE
    ${CMAKE_SOURCE_DIR}/LoupeLibCore/sources
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources
    ${CMAKE_BINARY_DIR}/${INSTALL_INCLUDEDIR}
)

set_target_properties(UnitTestsInteractionWidget PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsInteractionWidget "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsInteractionWidget")

add_executable(UnitTestsPageSurfaceProbe
    tst_pagesurfaceprobetest.cpp
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources/pdfinteractiontrace_p.cpp
)

target_link_libraries(UnitTestsPageSurfaceProbe PRIVATE
    LoupeLibWidgets
    LoupeLibCore
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
)
target_include_directories(UnitTestsPageSurfaceProbe PRIVATE
    ${CMAKE_SOURCE_DIR}/LoupeLibCore/sources
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources
    ${CMAKE_BINARY_DIR}/${INSTALL_INCLUDEDIR}
)

set_target_properties(UnitTestsPageSurfaceProbe PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsPageSurfaceProbe "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPageSurfaceProbe")

add_executable(UnitTestsOverlayRendering
    tst_overlayrenderingtest.cpp
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources/pdfinteractiontrace_p.cpp
)

target_link_libraries(UnitTestsOverlayRendering PRIVATE
    LoupeLibWidgets
    LoupeLibCore
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
)

target_include_directories(UnitTestsOverlayRendering PRIVATE
    ${CMAKE_SOURCE_DIR}/LoupeLibCore/sources
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources
    ${CMAKE_BINARY_DIR}/${INSTALL_INCLUDEDIR}
)

set_target_properties(UnitTestsOverlayRendering PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsOverlayRendering "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsOverlayRendering")

add_executable(UnitTestsPageSurfaceBudget
    tst_pagesurfacebudgettest.cpp
)

target_link_libraries(UnitTestsPageSurfaceBudget PRIVATE
    LoupeLibWidgets
    LoupeLibCore
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Test
)

target_include_directories(UnitTestsPageSurfaceBudget PRIVATE
    ${CMAKE_SOURCE_DIR}/LoupeLibCore/sources
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources
    ${CMAKE_BINARY_DIR}/${INSTALL_INCLUDEDIR}
)

set_target_properties(UnitTestsPageSurfaceBudget PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsPageSurfaceBudget
    "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPageSurfaceBudget"
)

add_executable(UnitTestsImageOptimizer
	tst_imageoptimizertest.cpp
)

target_link_libraries(UnitTestsImageOptimizer PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsImageOptimizer PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsImageOptimizer  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsImageOptimizer")

add_executable(UnitTestsImageDownsampleFixup
    tst_imagedownsamplefixuptest.cpp
)

target_link_libraries(UnitTestsImageDownsampleFixup PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsImageDownsampleFixup PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsImageDownsampleFixup "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsImageDownsampleFixup")

add_executable(UnitTestsBleedFixup
	tst_bleedfixuptest.cpp
)

target_link_libraries(UnitTestsBleedFixup PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

add_executable(UnitTestsDocumentSession
	tst_documentsessiontest.cpp
)

target_link_libraries(UnitTestsDocumentSession PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsDocumentSession PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)


add_executable(UnitTestsFontEncoding
	tst_fontencodingtest.cpp
)

target_link_libraries(UnitTestsFontEncoding PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsFontEncoding PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsFontEncoding  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsFontEncoding")

add_executable(UnitTestsAuthorSettings
	tst_authorsettingstest.cpp
)

target_link_libraries(UnitTestsAuthorSettings PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsAuthorSettings PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsAuthorSettings  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsAuthorSettings")

add_executable(UnitTestsDiagnostics
	tst_diagnosticstest.cpp
)

target_link_libraries(UnitTestsDiagnostics PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsDiagnostics PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
# Test name is lower-case (unlike sibling targets) so it matches
# `ctest -R diagnostics` as documented in docs/PRODUCTION_RUNBOOK.md.
add_test(diagnostics  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsDiagnostics")

add_executable(UnitTestsContentEditor
	tst_contenteditortest.cpp
)

target_link_libraries(UnitTestsContentEditor PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsContentEditor PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsContentEditor  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsContentEditor")


add_test(UnitTestsDocumentSession "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsDocumentSession")

add_executable(UnitTestsRepairDiff
    tst_repairdifftest.cpp
)

target_link_libraries(UnitTestsRepairDiff PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsRepairDiff PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsRepairDiff "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsRepairDiff")

add_executable(UnitTestsRepairOperation
    tst_repairoperationtest.cpp
)

target_link_libraries(UnitTestsRepairOperation PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsRepairOperation PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsRepairOperation "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsRepairOperation")

add_executable(UnitTestsActionList
    tst_actionlisttest.cpp
)

target_link_libraries(UnitTestsActionList PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsActionList PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsActionList "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsActionList")

add_executable(UnitTestsOperationHistory
    tst_operationhistorytest.cpp
)

target_link_libraries(UnitTestsOperationHistory PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Sql Qt6::Test)

set_target_properties(UnitTestsOperationHistory PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_executable(UnitTestsProductionGeometry
    tst_productiongeometrytest.cpp
)

target_link_libraries(UnitTestsProductionGeometry PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsProductionGeometry PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)

add_test(UnitTestsOperationHistory "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsOperationHistory")
add_test(UnitTestsProductionGeometry "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsProductionGeometry")

add_executable(UnitTestsPreflightEngine
	tst_preflightenginetest.cpp
)

target_link_libraries(UnitTestsPreflightEngine PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(UnitTestsPreflightEngine PRIVATE
    LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight")

set_target_properties(UnitTestsPreflightEngine PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsPreflightEngine "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPreflightEngine")

add_executable(UnitTestsPreflightVerdict
    tst_preflightverdicttest.cpp
)

target_link_libraries(UnitTestsPreflightVerdict PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsPreflightVerdict PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsPreflightVerdict "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPreflightVerdict")

add_executable(UnitTestsSchemaEvolution
    tst_schemaevolutiontest.cpp
)
target_link_libraries(UnitTestsSchemaEvolution PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(UnitTestsSchemaEvolution PRIVATE
    LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight")
set_target_properties(UnitTestsSchemaEvolution PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsSchemaEvolution "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsSchemaEvolution")

add_executable(UnitTestsEvidenceGraph
    tst_evidencegraphtest.cpp
)
target_link_libraries(UnitTestsEvidenceGraph PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(UnitTestsEvidenceGraph PRIVATE
    LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight")
set_target_properties(UnitTestsEvidenceGraph PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsEvidenceGraph "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsEvidenceGraph")

add_executable(UnitTestsProfileIdentity
    tst_preflightprofileidentitytest.cpp
)
target_link_libraries(UnitTestsProfileIdentity PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(UnitTestsProfileIdentity PRIVATE
    LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight"
    LOUPE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(UnitTestsProfileIdentity PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsProfileIdentity "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsProfileIdentity")

add_executable(UnitTestsOperationImpact
    tst_operationimpacttest.cpp
)
target_link_libraries(UnitTestsOperationImpact PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
set_target_properties(UnitTestsOperationImpact PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsOperationImpact "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsOperationImpact")

add_executable(UnitTestsStandardOracle
    tst_standardoracletest.cpp
)
target_link_libraries(UnitTestsStandardOracle PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
set_target_properties(UnitTestsStandardOracle PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsStandardOracle "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsStandardOracle")

add_executable(UnitTestsPluginAbi
    tst_pluginabitest.cpp
)
target_link_libraries(UnitTestsPluginAbi PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
set_target_properties(UnitTestsPluginAbi PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsPluginAbi "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPluginAbi")

add_executable(UnitTestsLifecycle
    tst_lifecycletest.cpp
)
target_link_libraries(UnitTestsLifecycle PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Sql Qt6::Test)
set_target_properties(UnitTestsLifecycle PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsLifecycle "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsLifecycle")

add_executable(UnitTestsBenchmarkIdentity
    tst_benchmarkidentitytest.cpp
)
target_link_libraries(UnitTestsBenchmarkIdentity PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
set_target_properties(UnitTestsBenchmarkIdentity PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsBenchmarkIdentity "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsBenchmarkIdentity")

add_executable(UnitTestsConversionOracle
    tst_conversionoracletest.cpp
)
target_link_libraries(UnitTestsConversionOracle PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(UnitTestsConversionOracle PRIVATE
    LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight")
set_target_properties(UnitTestsConversionOracle PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsConversionOracle "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsConversionOracle")

add_executable(UnitTestsHugeDocumentEnvelope
    tst_hugedocumentenvelopetest.cpp
)
target_link_libraries(UnitTestsHugeDocumentEnvelope PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
set_target_properties(UnitTestsHugeDocumentEnvelope PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsHugeDocumentEnvelope "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsHugeDocumentEnvelope")

add_executable(UnitTestsPreflightProfileResolver
    tst_preflightprofileresolvertest.cpp
)

target_link_libraries(UnitTestsPreflightProfileResolver PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsPreflightProfileResolver PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsPreflightProfileResolver "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPreflightProfileResolver")

add_executable(UnitTestsOcrPageGate
    tst_ocrpagegatetest.cpp
)

target_link_libraries(UnitTestsOcrPageGate PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(UnitTestsOcrPageGate PRIVATE
    LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight")

set_target_properties(UnitTestsOcrPageGate PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsOcrPageGate "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsOcrPageGate")

add_executable(UnitTestsOcrCli
    tst_ocrclitest.cpp
)

target_link_libraries(UnitTestsOcrCli PRIVATE Qt6::Core Qt6::Test)
add_dependencies(UnitTestsOcrCli PdfTool)
target_compile_definitions(UnitTestsOcrCli PRIVATE
    LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight"
    LOUPE_OCR_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-ocr"
    PDFTOOL_EXECUTABLE_PATH="$<TARGET_FILE:PdfTool>")

set_target_properties(UnitTestsOcrCli PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsOcrCli "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsOcrCli")

add_executable(UnitTestsOcrContract
    tst_ocrcontracttest.cpp
)

target_link_libraries(UnitTestsOcrContract PRIVATE Qt6::Core Qt6::Test)
target_include_directories(UnitTestsOcrContract PRIVATE
    ${CMAKE_SOURCE_DIR}/PdfTool
    ${CMAKE_SOURCE_DIR}/LoupeEditorPlugins/OcrPlugin)

set_target_properties(UnitTestsOcrContract PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsOcrContract "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsOcrContract")

add_executable(UnitTestsPdfToolContract
    tst_pdftoolcontract.cpp
)

target_link_libraries(UnitTestsPdfToolContract PRIVATE Qt6::Core Qt6::Test)
add_dependencies(UnitTestsPdfToolContract PdfTool)
target_compile_definitions(UnitTestsPdfToolContract PRIVATE
    PDFTOOL_EXECUTABLE_PATH="$<TARGET_FILE:PdfTool>")

set_target_properties(UnitTestsPdfToolContract PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsPdfToolContract "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPdfToolContract")

add_executable(UnitTestsBleedMarginProbe
	tst_bleedmarginprobetest.cpp
)

target_link_libraries(UnitTestsBleedMarginProbe PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsBleedMarginProbe PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsBleedMarginProbe "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsBleedMarginProbe")

set_target_properties(UnitTestsBleedFixup PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsBleedFixup  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsBleedFixup")

add_executable(UnitTestsRgbToCmyk
    tst_rgbtocmykfixup.cpp
)
target_link_libraries(UnitTestsRgbToCmyk PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
set_target_properties(UnitTestsRgbToCmyk PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsRgbToCmyk "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsRgbToCmyk")

add_executable(UnitTestsOverprint
	tst_overprinttest.cpp
)

target_link_libraries(UnitTestsOverprint PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsOverprint PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsOverprint "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsOverprint")

if(NOT LOUPE_BUILD_ONLY_CORE_LIBRARY)
    add_executable(UnitTestsOverprintRender
        tst_overprintrendertest.cpp
    )

    target_link_libraries(UnitTestsOverprintRender PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
    target_compile_definitions(UnitTestsOverprintRender PRIVATE
        LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight")

    set_target_properties(UnitTestsOverprintRender PROPERTIES
        WIN32_EXECUTABLE OFF
        MACOSX_BUNDLE OFF
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
    )
    add_test(UnitTestsOverprintRender "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsOverprintRender")
endif()

add_executable(UnitTestsPageMasterExport
	tst_pagemasterexporttest.cpp
)

target_link_libraries(UnitTestsPageMasterExport PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test Qt6::Concurrent)

target_compile_definitions(UnitTestsPageMasterExport PRIVATE
    LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight"
)

set_target_properties(UnitTestsPageMasterExport PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsPageMasterExport "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPageMasterExport")

add_executable(UnitTestsTransparencyFlattener
    tst_transparencyflattenertest.cpp
)

target_link_libraries(UnitTestsTransparencyFlattener PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsTransparencyFlattener PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsTransparencyFlattener "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsTransparencyFlattener")

add_executable(UnitTestsFilenameSanitizer
	tst_filenamesanitizertest.cpp
)

target_link_libraries(UnitTestsFilenameSanitizer PRIVATE LoupeLibCore Qt6::Core Qt6::Test)

set_target_properties(UnitTestsFilenameSanitizer PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsFilenameSanitizer "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsFilenameSanitizer")

add_executable(UnitTestsSafeFileWriter
	tst_safefilewritertest.cpp
)

target_link_libraries(UnitTestsSafeFileWriter PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsSafeFileWriter PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsSafeFileWriter "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsSafeFileWriter")

add_executable(UnitTestsContentProcessorLimits
	tst_contentprocessorlimitstest.cpp
)

target_link_libraries(UnitTestsContentProcessorLimits PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsContentProcessorLimits PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsContentProcessorLimits "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsContentProcessorLimits")

add_executable(UnitTestsJbig2Decoder
	tst_jbig2decodertest.cpp
)

target_link_libraries(UnitTestsJbig2Decoder PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsJbig2Decoder PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsJbig2Decoder "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsJbig2Decoder")

add_executable(UnitTestsRedactVerifier
    tst_redactverifiertest.cpp
)

target_link_libraries(UnitTestsRedactVerifier PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsRedactVerifier PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsRedactVerifier "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsRedactVerifier")

add_executable(UnitTestsThumbnailsRenderer
	tst_thumbnailsrenderertest.cpp
)

target_link_libraries(UnitTestsThumbnailsRenderer PRIVATE LoupeLibWidgets LoupeLibCore Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)

set_target_properties(UnitTestsThumbnailsRenderer PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsThumbnailsRenderer "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsThumbnailsRenderer")

# Pure geometry math for the preflight custom checks (bleed / trim / page-size).
# Header-only, so it only needs the PdfTool include dir - no Core or PdfTool link.
add_executable(UnitTestsPreflightChecks
	tst_preflightchecks.cpp
)

target_link_libraries(UnitTestsPreflightChecks PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
target_include_directories(UnitTestsPreflightChecks PRIVATE
    ${CMAKE_SOURCE_DIR}/PdfTool
    ${CMAKE_SOURCE_DIR}/LoupeLibCore/sources)

set_target_properties(UnitTestsPreflightChecks PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsPreflightChecks  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPreflightChecks")

# Locks the Editor plugin's pure sidecar path, exit-code, and report-contract helpers.
add_executable(UnitTestsPreflightPlugin
    tst_preflightplugintest.cpp
    ${CMAKE_SOURCE_DIR}/LoupeEditorPlugins/LoupePreflightPlugin/preflightreportmodel.cpp
)

target_link_libraries(UnitTestsPreflightPlugin PRIVATE Qt6::Core Qt6::Gui Qt6::Test LoupeLibCore LoupeLibWidgets)
target_include_directories(UnitTestsPreflightPlugin PRIVATE
    ${CMAKE_SOURCE_DIR}/LoupeEditorPlugins/LoupePreflightPlugin
    ${CMAKE_SOURCE_DIR}/LoupeEditorPlugins
    ${CMAKE_SOURCE_DIR}/LoupeLibCore/sources
    ${CMAKE_SOURCE_DIR}/LoupeLibWidgets/sources
    ${CMAKE_BINARY_DIR})
target_compile_definitions(UnitTestsPreflightPlugin PRIVATE
    LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight")

set_target_properties(UnitTestsPreflightPlugin PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsPreflightPlugin  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPreflightPlugin")

# Drives `PdfTool preflight` against the loupe-preflight golden corpus, so it needs
# the PdfTool executable, which is not built in LOUPE_BUILD_ONLY_CORE_LIBRARY mode.
if(NOT LOUPE_BUILD_ONLY_CORE_LIBRARY)
    add_executable(UnitTestsPreflightCorpus
        tst_preflightcorpus.cpp
    )

    target_link_libraries(UnitTestsPreflightCorpus PRIVATE Qt6::Core Qt6::Test)
    add_dependencies(UnitTestsPreflightCorpus PdfTool)
    target_compile_definitions(UnitTestsPreflightCorpus PRIVATE
        PDFTOOL_EXECUTABLE_PATH="$<TARGET_FILE:PdfTool>"
        LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight"
    )

    set_target_properties(UnitTestsPreflightCorpus PROPERTIES
        WIN32_EXECUTABLE OFF
        MACOSX_BUNDLE OFF
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
    )
    add_test(UnitTestsPreflightCorpus  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsPreflightCorpus")

    add_executable(UnitTestsBleedStress
        tst_bleedstresstest.cpp
    )

    target_link_libraries(UnitTestsBleedStress PRIVATE Qt6::Core Qt6::Test)
    add_dependencies(UnitTestsBleedStress PdfTool)
    target_compile_definitions(UnitTestsBleedStress PRIVATE
        PDFTOOL_EXECUTABLE_PATH="$<TARGET_FILE:PdfTool>"
        LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight"
    )

    set_target_properties(UnitTestsBleedStress PROPERTIES
        WIN32_EXECUTABLE OFF
        MACOSX_BUNDLE OFF
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
    )
    add_test(UnitTestsBleedStress  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsBleedStress")

    add_executable(UnitTestsOperatorAcceptance
        tst_operatoracceptance.cpp
    )

    target_link_libraries(UnitTestsOperatorAcceptance PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
    add_dependencies(UnitTestsOperatorAcceptance PdfTool)
    target_compile_definitions(UnitTestsOperatorAcceptance PRIVATE
        PDFTOOL_EXECUTABLE_PATH="$<TARGET_FILE:PdfTool>"
        LOUPE_PREFLIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/loupe-preflight"
    )
    target_include_directories(UnitTestsOperatorAcceptance PRIVATE
        ${CMAKE_SOURCE_DIR}/LoupeEditorPlugins/LoupePreflightPlugin
        ${CMAKE_SOURCE_DIR}/LoupeEditorPlugins
        ${CMAKE_SOURCE_DIR}/LoupeLibCore/sources
        ${CMAKE_BINARY_DIR}/${INSTALL_INCLUDEDIR})

    set_target_properties(UnitTestsOperatorAcceptance PROPERTIES
        WIN32_EXECUTABLE OFF
        MACOSX_BUNDLE OFF
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
    )
    add_test(UnitTestsOperatorAcceptance  "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsOperatorAcceptance")
endif()

# Compiles both identity headers into one TU so the pdf::PDFArtifactIdentity
# redefinition that broke the Core build cannot silently return.
add_executable(UnitTestsIdentitySeparation
	tst_identityseparationtest.cpp
)

target_link_libraries(UnitTestsIdentitySeparation PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)

set_target_properties(UnitTestsIdentitySeparation PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsIdentitySeparation "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsIdentitySeparation")

add_executable(UnitTestsBudgetExhaustion
    tst_budgetexhaustiontest.cpp
)
target_link_libraries(UnitTestsBudgetExhaustion PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
set_target_properties(UnitTestsBudgetExhaustion PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsBudgetExhaustion "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsBudgetExhaustion")

add_executable(UnitTestsWorkloadEnvelope
    tst_workloadenvelopetest.cpp
)
target_link_libraries(UnitTestsWorkloadEnvelope PRIVATE LoupeLibCore Qt6::Core Qt6::Gui Qt6::Test)
set_target_properties(UnitTestsWorkloadEnvelope PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_LIB_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}
)
add_test(UnitTestsWorkloadEnvelope "${CMAKE_BINARY_DIR}/${LOUPE_INSTALL_BIN_DIR}/UnitTestsWorkloadEnvelope")

# windeployqt copies Core/Gui/etc. but not Qt Test; all UnitTests* targets share one output dir.
if(WIN32)
    add_custom_command(TARGET UnitTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Qt6::Test>
            $<TARGET_FILE_DIR:UnitTests>
        COMMENT "Copy Qt6Test runtime for unit tests")
endif()
