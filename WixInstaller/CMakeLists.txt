# MIT License
#
# The Windows installer contains only the current LoopEditor and PdfTool
# product boundary. Retired Widgets products and plugins are deliberately not
# represented here, even when a developer build happens to contain old files.

if(WIN32 AND MSVC AND LOOP_INSTALL_PREPARE_WIX_INSTALLER)
    if(LOOP_INSTALL_TO_USR)
        file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}/usr/bin" WIX_INSTALL_PREFIX)
    else()
        file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}/bin" WIX_INSTALL_PREFIX)
    endif()
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/WixInstaller" LOOP_WIX_DIR)

    # Product data is part of the one Editor feature. There is no second
    # Widgets/plugin feature tree: the product-surface manifest is the source
    # of truth for the release inventory.
    set(LOOP_WIX_EDITOR_DATA_REF [=[
        <ComponentGroupRef Id="LOOP_Loop_Data"/>
]=])
    string(CONFIGURE "${LOOP_WIX_EDITOR_DATA_REF}" LOOP_WIX_EDITOR_DATA_REF @ONLY)

    if(LOOP_BUNDLE_OCR_SERVICE)
        set(LOOP_WIX_FEATURE_OCR_SERVICE [=[
          <ComponentGroupRef Id="LOOP_Ocr_Service"/>
]=])
        set(LOOP_WIX_FRAGMENT_OCR_SERVICE [=[
  <Fragment>
    <DirectoryRef Id="INSTALLFOLDER">
      <Directory Id="dir_LoopOcrService" Name="LoopOcrService" />
    </DirectoryRef>
  </Fragment>

  <Fragment>
    <ComponentGroup Id="LOOP_Ocr_Service">
      <Component Id="cmpLoopOcrServiceExe" Directory="dir_LoopOcrService" Guid="{D9A6C3E4-5F7B-4A02-9C3D-4E0F9A8B7C62}">
        <File Id="filLoopOcrServiceExe" KeyPath="yes" Source="$(var.MyInstallDir)\LoopOcrService\LoopOcrService.exe" />
      </Component>
    </ComponentGroup>
  </Fragment>
]=])
    else()
        set(LOOP_WIX_FEATURE_OCR_SERVICE "")
        set(LOOP_WIX_FRAGMENT_OCR_SERVICE "")
    endif()
    string(CONFIGURE "${LOOP_WIX_FEATURE_OCR_SERVICE}" LOOP_WIX_FEATURE_OCR_SERVICE @ONLY)

    if(LOOP_ENABLE_SENTRY)
        set(LOOP_WIX_SENTRY_CRASHPAD [=[
      <Component Id="cmpCrashpadHandler" Directory="INSTALLFOLDER" Guid="{F4A2C8E1-6B3D-4F90-9C1A-2E7D5B8A4F31}">
        <File Id="filCrashpadHandler" KeyPath="yes" Source="$(var.MyInstallDir)\crashpad_handler.exe" />
      </Component>
]=])
    else()
        set(LOOP_WIX_SENTRY_CRASHPAD "")
    endif()

    # Qt ICU DLLs have versioned filenames. Discover them at configure time so
    # the MSI matches cmake --install when Qt dependencies are staged.

    # The release product is Quick-only and must not ship or reference the
    # Widgets runtime. Keep the legacy component available for developer
    # installer experiments, but make the distribution decision explicit in
    # the generated WiX source rather than relying on the staged directory.
    if(LOOP_LOOP_DISTRIBUTION)
        set(LOOP_WIX_QT_WIDGETS_COMPONENT "")
        set(LOOP_WIX_QT_PRINTSUPPORT_COMPONENT "")
        set(LOOP_WIX_QT_PRINTSUPPORT_DIRECTORY "")
        set(LOOP_WIX_QT_STYLES_COMPONENT "")
        set(LOOP_WIX_QT_STYLES_DIRECTORY "")
    else()
        set(LOOP_WIX_QT_WIDGETS_COMPONENT [=[
      <Component Id="cmpQt6Widgets" Directory="INSTALLFOLDER" Guid="{C686E727-51FF-4A40-A7FE-E1A2130FDAAF}">
                <File Id="filQt6Widgets" KeyPath="yes" Source="$(var.MyInstallDir)\Qt6Widgets.dll" />
      </Component>
]=])
        set(LOOP_WIX_QT_PRINTSUPPORT_COMPONENT [=[
      <Component Id="cmpQt6PrintSupport" Directory="INSTALLFOLDER" Guid="{C8045343-5FD3-4349-B4E6-B770F4D28381}">
                <File Id="filQt6PrintSupport" KeyPath="yes" Source="$(var.MyInstallDir)\Qt6PrintSupport.dll" />
      </Component>
]=])
        set(LOOP_WIX_QT_PRINTSUPPORT_DIRECTORY [=[
  <Fragment>
    <DirectoryRef Id="INSTALLFOLDER">
      <Directory Id="dir_printsupport" Name="printsupport" />
    </DirectoryRef>
  </Fragment>
]=])
        set(LOOP_WIX_QT_STYLES_COMPONENT [=[
      <Component Id="cmpqmodernwindowsstyle" Directory="dir_styles" Guid="{1A418DAB-7A24-4695-A773-4DC161926350}">
        <File Id="filqmodernwindowsstyle" KeyPath="yes" Source="$(var.MyInstallDir)\styles\qmodernwindowsstyle.dll" />
      </Component>
]=])
        set(LOOP_WIX_QT_STYLES_DIRECTORY [=[
  <Fragment>
    <DirectoryRef Id="INSTALLFOLDER">
      <Directory Id="dir_styles" Name="styles" />
    </DirectoryRef>
  </Fragment>
]=])
    endif()

    set(LOOP_WIX_ICU_COMPONENTS "")

    if(LOOP_INSTALL_QT_DEPENDENCIES AND NOT "${LOOP_QT_ROOT}" STREQUAL "")
        file(GLOB _loop_icu_dlls LIST_DIRECTORIES false "${LOOP_QT_ROOT}/bin/icu*.dll")
        set(_loop_icu_index 0)
        foreach(_loop_icu_dll IN LISTS _loop_icu_dlls)
            get_filename_component(_loop_icu_name "${_loop_icu_dll}" NAME)
            get_filename_component(_loop_icu_id "${_loop_icu_dll}" NAME_WE)
            string(REGEX REPLACE "[^A-Za-z0-9]" "" _loop_icu_id "${_loop_icu_id}")
            math(EXPR _loop_icu_index "${_loop_icu_index}+1")
            string(UUID _loop_icu_guid NAMESPACE "6ba7b810-9dad-11d1-80b4-00c04fd430c8"
                   NAME "loop-wix-icu-${_loop_icu_name}" TYPE SHA1 UPPER)
            string(APPEND LOOP_WIX_ICU_COMPONENTS
"      <Component Id=\"cmp${_loop_icu_id}\" Directory=\"INSTALLFOLDER\" Guid=\"{${_loop_icu_guid}}\">
        <File Id=\"fil${_loop_icu_id}\" KeyPath=\"yes\" Source=\"\$(var.MyInstallDir)\\${_loop_icu_name}\" />
      </Component>
")
        endforeach()
        if(_loop_icu_index EQUAL 0)
            message(WARNING "No icu*.dll found under ${LOOP_QT_ROOT}/bin")
        endif()
    endif()

    # cmake --install stages the complete Qt QML tree below qml/. WiX only
    # packages files named in Product.wxs, so harvest the same tree at
    # configure time and keep every generated component tied to its relative
    # source path. The deterministic UUIDs keep upgrades stable across runs.
    set(LOOP_WIX_QML_DIRECTORY_REFS "")
    set(LOOP_WIX_QML_COMPONENTS "")
    if(LOOP_INSTALL_QT_DEPENDENCIES AND NOT "${LOOP_QT_ROOT}" STREQUAL "")
        set(_loop_qml_root "${LOOP_QT_ROOT}/qml")
        if(IS_DIRECTORY "${_loop_qml_root}")
            set(LOOP_WIX_QML_DIRECTORY_REFS [=[
  <Fragment>
    <DirectoryRef Id="INSTALLFOLDER">
      <Directory Id="dir_qml" Name="qml" />
    </DirectoryRef>
  </Fragment>
]=])
            file(GLOB_RECURSE _loop_qml_files LIST_DIRECTORIES false "${_loop_qml_root}/*")
            list(SORT _loop_qml_files)
            set(_loop_qml_dirs "")
            foreach(_loop_qml_file IN LISTS _loop_qml_files)
                file(RELATIVE_PATH _loop_qml_relative "${_loop_qml_root}" "${_loop_qml_file}")
                get_filename_component(_loop_qml_dir "${_loop_qml_relative}" DIRECTORY)
                while(NOT "${_loop_qml_dir}" STREQUAL "" AND NOT "${_loop_qml_dir}" STREQUAL ".")
                    list(FIND _loop_qml_dirs "${_loop_qml_dir}" _loop_qml_dir_index)
                    if(_loop_qml_dir_index EQUAL -1)
                        list(APPEND _loop_qml_dirs "${_loop_qml_dir}")
                    endif()
                    get_filename_component(_loop_qml_dir "${_loop_qml_dir}" DIRECTORY)
                endwhile()
            endforeach()

            list(SORT _loop_qml_dirs)
            foreach(_loop_qml_dir IN LISTS _loop_qml_dirs)
                string(MD5 _loop_qml_dir_hash "${_loop_qml_dir}")
                string(SUBSTRING "${_loop_qml_dir_hash}" 0 16 _loop_qml_dir_hash)
                set(_loop_qml_dir_id "dirQml${_loop_qml_dir_hash}")
                get_filename_component(_loop_qml_dir_name "${_loop_qml_dir}" NAME)
                string(REPLACE "&" "&amp;" _loop_qml_dir_name "${_loop_qml_dir_name}")
                string(REPLACE "<" "&lt;" _loop_qml_dir_name "${_loop_qml_dir_name}")
                string(REPLACE ">" "&gt;" _loop_qml_dir_name "${_loop_qml_dir_name}")
                get_filename_component(_loop_qml_parent "${_loop_qml_dir}" DIRECTORY)
                if("${_loop_qml_parent}" STREQUAL "")
                    set(_loop_qml_parent_id "dir_qml")
                else()
                    string(MD5 _loop_qml_parent_hash "${_loop_qml_parent}")
                    string(SUBSTRING "${_loop_qml_parent_hash}" 0 16 _loop_qml_parent_hash)
                    set(_loop_qml_parent_id "dirQml${_loop_qml_parent_hash}")
                endif()
                string(APPEND LOOP_WIX_QML_DIRECTORY_REFS
                    "  <Fragment>\n"
                    "    <DirectoryRef Id=\"${_loop_qml_parent_id}\">\n"
                    "      <Directory Id=\"${_loop_qml_dir_id}\" Name=\"${_loop_qml_dir_name}\" />\n"
                    "    </DirectoryRef>\n"
                    "  </Fragment>\n")
            endforeach()

            foreach(_loop_qml_file IN LISTS _loop_qml_files)
                file(RELATIVE_PATH _loop_qml_relative "${_loop_qml_root}" "${_loop_qml_file}")
                get_filename_component(_loop_qml_dir "${_loop_qml_relative}" DIRECTORY)
                if("${_loop_qml_dir}" STREQUAL "")
                    set(_loop_qml_dir_id "dir_qml")
                else()
                    string(MD5 _loop_qml_dir_hash "${_loop_qml_dir}")
                    string(SUBSTRING "${_loop_qml_dir_hash}" 0 16 _loop_qml_dir_hash)
                    set(_loop_qml_dir_id "dirQml${_loop_qml_dir_hash}")
                endif()
                string(MD5 _loop_qml_file_hash "${_loop_qml_relative}")
                string(SUBSTRING "${_loop_qml_file_hash}" 0 16 _loop_qml_file_hash)
                string(UUID _loop_qml_guid
                       NAMESPACE "6ba7b810-9dad-11d1-80b4-00c04fd430c8"
                       NAME "loop-wix-qml-${_loop_qml_relative}" TYPE SHA1 UPPER)
                string(REPLACE "&" "&amp;" _loop_qml_source "${_loop_qml_relative}")
                string(REPLACE "<" "&lt;" _loop_qml_source "${_loop_qml_source}")
                string(REPLACE ">" "&gt;" _loop_qml_source "${_loop_qml_source}")
                string(APPEND LOOP_WIX_QML_COMPONENTS
                    "      <Component Id=\"cmpQml${_loop_qml_file_hash}\" Directory=\"${_loop_qml_dir_id}\" Guid=\"{${_loop_qml_guid}}\">\n"
                    "        <File Id=\"filQml${_loop_qml_file_hash}\" KeyPath=\"yes\" Source=\"$(var.MyInstallDir)/qml/${_loop_qml_source}\" />\n"
                    "      </Component>\n")
            endforeach()
            list(LENGTH _loop_qml_files _loop_qml_file_count)
            message(STATUS "WiX will package ${_loop_qml_file_count} Qt QML file(s) from ${_loop_qml_root}")
        else()
            message(WARNING "Qt QML root does not exist at ${_loop_qml_root}; MSI will not contain QML modules")
        endif()
    endif()

    configure_file(LOOP.sln.in LOOP.sln)
    configure_file(LOOP.wixproj.in LOOP.wixproj)
    configure_file(Product.wxs.in Product.wxs)
    configure_file(createInstaller.ps1.in createInstaller.ps1)
    configure_file(explic.rtf explic.rtf)
endif()
