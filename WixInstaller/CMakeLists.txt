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

if(WIN32 AND MSVC AND LOUPE_INSTALL_PREPARE_WIX_INSTALLER)
    if(LOUPE_INSTALL_TO_USR)
        file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}/usr/bin" WIX_INSTALL_PREFIX)
    else()
        file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}/bin" WIX_INSTALL_PREFIX)
    endif()
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/WixInstaller" LOUPE_WIX_DIR)

    if(LOUPE_LOUPE_DISTRIBUTION)
        set(LOUPE_WIX_PLUGIN_PACK_DESCRIPTION "LoupeEditor Quick shell; Widgets editor plugins were retired in Phase 5 Issue 17.")
        # Keep compatibility binaries in the release installer, but do not
        # present them as separate Loupe products in the WiX feature tree.
        set(LOUPE_WIX_COMPAT_FEATURE_DISPLAY [=[Display="hidden"]=])
    elseif(LOUPE_PLUGIN_AUDIOBOOK)
        set(LOUPE_WIX_PLUGIN_PACK_DESCRIPTION "LoupeEditor Quick shell; Widgets editor plugins were retired in Phase 5 Issue 17.")
        set(LOUPE_WIX_COMPAT_FEATURE_DISPLAY "")
    else()
        set(LOUPE_WIX_PLUGIN_PACK_DESCRIPTION "LoupeEditor Quick shell; Widgets editor plugins were retired in Phase 5 Issue 17.")
        set(LOUPE_WIX_COMPAT_FEATURE_DISPLAY "")
    endif()

    set(LOUPE_WIX_FRAMEWORK_WIDGETS_LIBS "")
    set(LOUPE_WIX_EDITOR_PLUGINS "")

    # The release product is Quick-only and must not ship or reference the
    # Widgets runtime. Keep the legacy component available for developer
    # installer experiments, but make the distribution decision explicit in
    # the generated WiX source rather than relying on the staged directory.
    if(LOUPE_LOUPE_DISTRIBUTION)
        set(LOUPE_WIX_QT_WIDGETS_COMPONENT "")
        set(LOUPE_WIX_QT_PRINTSUPPORT_COMPONENT "")
        set(LOUPE_WIX_QT_PRINTSUPPORT_DIRECTORY "")
    else()
        set(LOUPE_WIX_QT_WIDGETS_COMPONENT [=[
      <Component Id="cmpQt6Widgets" Directory="INSTALLFOLDER" Guid="{C686E727-51FF-4A40-A7FE-E1A2130FDAAF}">
                <File Id="filQt6Widgets" KeyPath="yes" Source="$(var.MyInstallDir)\Qt6Widgets.dll" />
      </Component>
]=])
        set(LOUPE_WIX_QT_PRINTSUPPORT_COMPONENT [=[
      <Component Id="cmpQt6PrintSupport" Directory="INSTALLFOLDER" Guid="{C8045343-5FD3-4349-B4E6-B770F4D28381}">
                <File Id="filQt6PrintSupport" KeyPath="yes" Source="$(var.MyInstallDir)\Qt6PrintSupport.dll" />
      </Component>
]=])
        set(LOUPE_WIX_QT_PRINTSUPPORT_DIRECTORY [=[
  <Fragment>
    <DirectoryRef Id="INSTALLFOLDER">
      <Directory Id="dir_printsupport" Name="printsupport" />
    </DirectoryRef>
  </Fragment>
]=])
    endif()

    if(LOUPE_LOUPE_DISTRIBUTION)
        set(LOUPE_WIX_VIEWER_SHORTCUT "")
        set(LOUPE_WIX_VIEWER_ASSOCIATION "")
        set(LOUPE_WIX_PAGEMASTER_SHORTCUT "")
        set(LOUPE_WIX_DIFF_SHORTCUT "")
    else()
        set(LOUPE_WIX_VIEWER_SHORTCUT [=[<Shortcut Id="LOUPE_Viewer_Shortcut" Directory="ProgramMenuDir" Name="LOUPE Viewer" Description="Simple PDF viewer" WorkingDirectory="INSTALLFOLDER" Icon="LOUPE_VL_Icon.exe" IconIndex="0" Advertise="yes"/>]=])
        set(LOUPE_WIX_VIEWER_ASSOCIATION [=[
        <ProgId Id='loupe_default_viewer.pdf' Description='PDF Document'>
          <Extension Id='pdf' ContentType='application/pdf'>
            <Verb Id='open' Command='Open' TargetFile='filLoupeViewer' Argument='"%1"' />
          </Extension>
        </ProgId>]=])
        set(LOUPE_WIX_PAGEMASTER_SHORTCUT [=[<Shortcut Id="LOUPE_DPO_Shortcut" Directory="ProgramMenuDir" Name="LOUPE PageMaster" Description="PDF Document page organizer" WorkingDirectory="INSTALLFOLDER" Icon="LOUPE_DPO_Icon.exe" IconIndex="0" Advertise="yes"/>]=])
        set(LOUPE_WIX_DIFF_SHORTCUT [=[<Shortcut Id="LOUPE_DD_Shortcut" Directory="ProgramMenuDir" Name="LOUPE Diff" Description="Compare two similar PDF documents" WorkingDirectory="INSTALLFOLDER" Icon="LOUPE_DD_Icon.exe" IconIndex="0" Advertise="yes"/>]=])
    endif()

    if(LOUPE_BUILD_VIEWER)
        set(LOUPE_WIX_FEATURE_VIEWER [=[
      <Feature Id="feature_LOUPE_Viewer" Absent="allow" AllowAdvertise="no" Title="Viewer" @LOUPE_WIX_COMPAT_FEATURE_DISPLAY@ Level="1" Description="Simple PDF viewer with basic functions.">
        <ComponentGroupRef Id="LOUPE_Viewer"/>
      </Feature>
]=])
        set(LOUPE_WIX_FRAGMENT_VIEWER [=[
  <Fragment>
    <ComponentGroup Id="LOUPE_Viewer">
      <Component Id="cmpLoupeViewer" Directory="INSTALLFOLDER" Guid="{2D222775-6DA7-43E6-B212-3A142622190C}">
        <File Id="filLoupeViewer" KeyPath="yes" Source="$(var.MyInstallDir)\LoupeViewer.exe">
          @LOUPE_WIX_VIEWER_SHORTCUT@
        </File>
        @LOUPE_WIX_VIEWER_ASSOCIATION@
      </Component>
    </ComponentGroup>
  </Fragment>

  <Fragment>
    <Icon Id="LOUPE_VL_Icon.exe" SourceFile="$(var.MyInstallDir)\LoupeViewer.exe"/>
  </Fragment>
]=])
    else()
        set(LOUPE_WIX_FEATURE_VIEWER "")
        set(LOUPE_WIX_FRAGMENT_VIEWER "")
    endif()

    if(LOUPE_BUILD_PAGEMASTER)
        set(LOUPE_WIX_FEATURE_PAGEMASTER [=[
      <Feature Id="feature_LOUPE_PageMaster" Absent="allow" AllowAdvertise="no" Title="PageMaster" @LOUPE_WIX_COMPAT_FEATURE_DISPLAY@ Level="1" Description="Document page organizer (split/merge documents, insert/remove/move/clone pages, insert blank pages and images to create a new document).">
        <ComponentGroupRef Id="LOUPE_PageMaster"/>
      </Feature>
]=])
        set(LOUPE_WIX_FRAGMENT_PAGEMASTER [=[
  <Fragment>
    <ComponentGroup Id="LOUPE_PageMaster">
      <Component Id="cmpLoupePageMaster" Directory="INSTALLFOLDER" Guid="{9D248BEE-BA78-4050-B362-0694B11AA326}">
        <File Id="filLoupePageMaster" KeyPath="yes" Source="$(var.MyInstallDir)\LoupePageMaster.exe">
          @LOUPE_WIX_PAGEMASTER_SHORTCUT@
        </File>
      </Component>
    </ComponentGroup>
  </Fragment>

  <Fragment>
    <Icon Id="LOUPE_DPO_Icon.exe" SourceFile="$(var.MyInstallDir)\LoupePageMaster.exe"/>
  </Fragment>
]=])
    else()
        set(LOUPE_WIX_FEATURE_PAGEMASTER "")
        set(LOUPE_WIX_FRAGMENT_PAGEMASTER "")
    endif()

    if(LOUPE_BUILD_DIFF)
        set(LOUPE_WIX_FEATURE_DIFF [=[
      <Feature Id="feature_LOUPE_Diff" Absent="allow" AllowAdvertise="no" Title="Diff" @LOUPE_WIX_COMPAT_FEATURE_DISPLAY@ Level="1" Description="Compare content of two documents.">
        <ComponentGroupRef Id="LOUPE_Diff"/>
      </Feature>
]=])
        set(LOUPE_WIX_FRAGMENT_DIFF [=[
  <Fragment>
    <ComponentGroup Id="LOUPE_Diff">
      <Component Id="cmpLoupeDiff" Directory="INSTALLFOLDER" Guid="{067D40E9-4FAE-472D-BCC3-B92A23CA0028}">
        <File Id="filLoupeDiff" KeyPath="yes" Source="$(var.MyInstallDir)\LoupeDiff.exe">
          @LOUPE_WIX_DIFF_SHORTCUT@
        </File>
      </Component>
    </ComponentGroup>
  </Fragment>

  <Fragment>
    <Icon Id="LOUPE_DD_Icon.exe" SourceFile="$(var.MyInstallDir)\LoupeDiff.exe"/>
  </Fragment>
]=])
    else()
        set(LOUPE_WIX_FEATURE_DIFF "")
        set(LOUPE_WIX_FRAGMENT_DIFF "")
    endif()

    string(CONFIGURE "${LOUPE_WIX_FEATURE_VIEWER}" LOUPE_WIX_FEATURE_VIEWER @ONLY)
    string(CONFIGURE "${LOUPE_WIX_FEATURE_PAGEMASTER}" LOUPE_WIX_FEATURE_PAGEMASTER @ONLY)
    string(CONFIGURE "${LOUPE_WIX_FEATURE_DIFF}" LOUPE_WIX_FEATURE_DIFF @ONLY)
    string(CONFIGURE "${LOUPE_WIX_FRAGMENT_VIEWER}" LOUPE_WIX_FRAGMENT_VIEWER @ONLY)
    string(CONFIGURE "${LOUPE_WIX_FRAGMENT_PAGEMASTER}" LOUPE_WIX_FRAGMENT_PAGEMASTER @ONLY)
    string(CONFIGURE "${LOUPE_WIX_FRAGMENT_DIFF}" LOUPE_WIX_FRAGMENT_DIFF @ONLY)

    if(LOUPE_BUILD_LAUNCHPAD)
        set(LOUPE_WIX_COMPAT_LAUNCHPAD [=[
      <Component Id="cmpLoupeLaunchPad" Directory="INSTALLFOLDER" Guid="{A7B8C9D0-E1F2-4A3B-8C4D-5E6F708192A3}">
        <File Id="filLoupeLaunchPad" KeyPath="yes" Source="$(var.MyInstallDir)\LoupeLaunchPad.exe" />
      </Component>
]=])
    else()
        set(LOUPE_WIX_COMPAT_LAUNCHPAD "")
    endif()

    if(LOUPE_PLUGIN_AUDIOBOOK)
        set(LOUPE_WIX_PLUGIN_AUDIOBOOK [=[
      <Component Id="cmpAudioBookPlugin" Directory="dir_pdfplugins" Guid="{06D159D2-0F34-417E-9134-7E9195E697A0}">
        <File Id="filAudioBookPlugin" KeyPath="yes" Source="$(var.MyInstallDir)\pdfplugins\AudioBookPlugin.dll" />
      </Component>
]=])
    else()
        set(LOUPE_WIX_PLUGIN_AUDIOBOOK "")
    endif()

    if(LOUPE_PLUGIN_SCANNER)
        set(LOUPE_WIX_PLUGIN_SCANNER [=[
      <Component Id="cmpScannerPlugin" Directory="dir_pdfplugins" Guid="{B7E4A1C2-3D5F-4E89-9A1B-2C8D7E6F5A40}">
        <File Id="filScannerPlugin" KeyPath="yes" Source="$(var.MyInstallDir)\pdfplugins\ScannerPlugin.dll" />
      </Component>
]=])
    else()
        set(LOUPE_WIX_PLUGIN_SCANNER "")
    endif()

    if(LOUPE_PLUGIN_OCR)
        set(LOUPE_WIX_PLUGIN_OCR [=[
      <Component Id="cmpOcrPlugin" Directory="dir_pdfplugins" Guid="{C8F5B2D3-4E6A-4F91-8B2C-3D9E8F7A6B51}">
        <File Id="filOcrPlugin" KeyPath="yes" Source="$(var.MyInstallDir)\pdfplugins\OcrPlugin.dll" />
      </Component>
]=])
    else()
        set(LOUPE_WIX_PLUGIN_OCR "")
    endif()

    if(LOUPE_BUNDLE_OCR_SERVICE)
        set(LOUPE_WIX_FEATURE_OCR_SERVICE [=[
          <ComponentGroupRef Id="LOUPE_Ocr_Service"/>
]=])
        set(LOUPE_WIX_FRAGMENT_OCR_SERVICE [=[
  <Fragment>
    <DirectoryRef Id="INSTALLFOLDER">
      <Directory Id="dir_LoupeOcrService" Name="LoupeOcrService" />
    </DirectoryRef>
  </Fragment>

  <Fragment>
    <ComponentGroup Id="LOUPE_Ocr_Service">
      <Component Id="cmpLoupeOcrServiceExe" Directory="dir_LoupeOcrService" Guid="{D9A6C3E4-5F7B-4A02-9C3D-4E0F9A8B7C62}">
        <File Id="filLoupeOcrServiceExe" KeyPath="yes" Source="$(var.MyInstallDir)\LoupeOcrService\LoupeOcrService.exe" />
      </Component>
    </ComponentGroup>
  </Fragment>
]=])
    else()
        set(LOUPE_WIX_FEATURE_OCR_SERVICE "")
        set(LOUPE_WIX_FRAGMENT_OCR_SERVICE "")
    endif()

    set(LOUPE_WIX_EDITOR_PLUGIN_FEATURE [=[
        <ComponentGroupRef Id="LOUPE_Loupe_Data"/>
@LOUPE_WIX_FEATURE_OCR_SERVICE@
]=])
    string(CONFIGURE "${LOUPE_WIX_EDITOR_PLUGIN_FEATURE}" LOUPE_WIX_EDITOR_PLUGIN_FEATURE @ONLY)

    if(LOUPE_ENABLE_SENTRY)
        set(LOUPE_WIX_SENTRY_CRASHPAD [=[
      <Component Id="cmpCrashpadHandler" Directory="INSTALLFOLDER" Guid="{F4A2C8E1-6B3D-4F90-9C1A-2E7D5B8A4F31}">
        <File Id="filCrashpadHandler" KeyPath="yes" Source="$(var.MyInstallDir)\crashpad_handler.exe" />
      </Component>
]=])
    else()
        set(LOUPE_WIX_SENTRY_CRASHPAD "")
    endif()

    # Qt ICU DLLs (versioned filenames vary by Qt build). Discover at configure time
    # and emit WiX components so the MSI matches cmake --install.
    set(LOUPE_WIX_ICU_COMPONENTS "")
    if(LOUPE_INSTALL_QT_DEPENDENCIES AND NOT "${LOUPE_QT_ROOT}" STREQUAL "")
        file(GLOB _loupe_icu_dlls LIST_DIRECTORIES false "${LOUPE_QT_ROOT}/bin/icu*.dll")
        set(_loupe_icu_index 0)
        foreach(_loupe_icu_dll IN LISTS _loupe_icu_dlls)
            get_filename_component(_loupe_icu_name "${_loupe_icu_dll}" NAME)
            get_filename_component(_loupe_icu_id "${_loupe_icu_dll}" NAME_WE)
            string(REGEX REPLACE "[^A-Za-z0-9]" "" _loupe_icu_id "${_loupe_icu_id}")
            math(EXPR _loupe_icu_index "${_loupe_icu_index}+1")
            # Deterministic GUID namespace from the DLL basename so reconfigure is stable.
            string(UUID _loupe_icu_guid NAMESPACE "6ba7b810-9dad-11d1-80b4-00c04fd430c8"
                   NAME "loupe-wix-icu-${_loupe_icu_name}" TYPE SHA1 UPPER)
            string(APPEND LOUPE_WIX_ICU_COMPONENTS
"      <Component Id=\"cmp${_loupe_icu_id}\" Directory=\"INSTALLFOLDER\" Guid=\"{${_loupe_icu_guid}}\">
        <File Id=\"fil${_loupe_icu_id}\" KeyPath=\"yes\" Source=\"$(var.MyInstallDir)\\${_loupe_icu_name}\" />
      </Component>
")
        endforeach()
        if(_loupe_icu_index EQUAL 0)
            message(WARNING "LOUPE_INSTALL_QT_DEPENDENCIES is ON but no icu*.dll found under ${LOUPE_QT_ROOT}/bin — MSI may fail at runtime with STATUS_DLL_NOT_FOUND")
        else()
            message(STATUS "WiX will package ${_loupe_icu_index} Qt ICU DLL(s) from ${LOUPE_QT_ROOT}/bin")
        endif()
    endif()

    configure_file(LOUPE.sln.in LOUPE.sln)
    configure_file(LOUPE.wixproj.in LOUPE.wixproj)
    configure_file(Product.wxs.in Product.wxs)
    configure_file(createInstaller.ps1.in createInstaller.ps1)
    configure_file(explic.rtf explic.rtf)
endif()
