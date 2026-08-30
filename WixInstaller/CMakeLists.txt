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

    # Product data is part of the one Editor feature. There is no second
    # Widgets/plugin feature tree: the product-surface manifest is the source
    # of truth for the release inventory.
    set(LOUPE_WIX_EDITOR_DATA_REF [=[
        <ComponentGroupRef Id="LOUPE_Loupe_Data"/>
]=])
    string(CONFIGURE "${LOUPE_WIX_EDITOR_DATA_REF}" LOUPE_WIX_EDITOR_DATA_REF @ONLY)

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
    string(CONFIGURE "${LOUPE_WIX_FEATURE_OCR_SERVICE}" LOUPE_WIX_FEATURE_OCR_SERVICE @ONLY)

    if(LOUPE_ENABLE_SENTRY)
        set(LOUPE_WIX_SENTRY_CRASHPAD [=[
      <Component Id="cmpCrashpadHandler" Directory="INSTALLFOLDER" Guid="{F4A2C8E1-6B3D-4F90-9C1A-2E7D5B8A4F31}">
        <File Id="filCrashpadHandler" KeyPath="yes" Source="$(var.MyInstallDir)\crashpad_handler.exe" />
      </Component>
]=])
    else()
        set(LOUPE_WIX_SENTRY_CRASHPAD "")
    endif()

    # Qt ICU DLLs have versioned filenames. Discover them at configure time so
    # the MSI matches cmake --install when Qt dependencies are staged.
    set(LOUPE_WIX_ICU_COMPONENTS "")
    if(LOUPE_INSTALL_QT_DEPENDENCIES AND NOT "${LOUPE_QT_ROOT}" STREQUAL "")
        file(GLOB _loupe_icu_dlls LIST_DIRECTORIES false "${LOUPE_QT_ROOT}/bin/icu*.dll")
        set(_loupe_icu_index 0)
        foreach(_loupe_icu_dll IN LISTS _loupe_icu_dlls)
            get_filename_component(_loupe_icu_name "${_loupe_icu_dll}" NAME)
            get_filename_component(_loupe_icu_id "${_loupe_icu_dll}" NAME_WE)
            string(REGEX REPLACE "[^A-Za-z0-9]" "" _loupe_icu_id "${_loupe_icu_id}")
            math(EXPR _loupe_icu_index "${_loupe_icu_index}+1")
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
