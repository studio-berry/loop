# MIT License
#
# The Windows installer contains only the current LoupeEditor and PdfTool
# product boundary. Retired Widgets products and plugins are deliberately not
# represented here, even when a developer build happens to contain old files.

if(WIN32 AND MSVC AND LOUPE_INSTALL_PREPARE_WIX_INSTALLER)
    if(LOUPE_INSTALL_TO_USR)
        file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}/usr/bin" WIX_INSTALL_PREFIX)
    else()
        file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}/bin" WIX_INSTALL_PREFIX)
    endif()
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/WixInstaller" LOUPE_WIX_DIR)

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
        set(LOUPE_WIX_QT_STYLES_COMPONENT "")
        set(LOUPE_WIX_QT_STYLES_DIRECTORY "")
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
        set(LOUPE_WIX_QT_STYLES_COMPONENT [=[
      <Component Id="cmpqmodernwindowsstyle" Directory="dir_styles" Guid="{1A418DAB-7A24-4695-A773-4DC161926350}">
        <File Id="filqmodernwindowsstyle" KeyPath="yes" Source="$(var.MyInstallDir)\styles\qmodernwindowsstyle.dll" />
      </Component>
]=])
        set(LOUPE_WIX_QT_STYLES_DIRECTORY [=[
  <Fragment>
    <DirectoryRef Id="INSTALLFOLDER">
      <Directory Id="dir_styles" Name="styles" />
    </DirectoryRef>
  </Fragment>
]=])
    endif()

    set(LOUPE_WIX_EDITOR_PLUGIN_FEATURE "<ComponentGroupRef Id=\"LOUPE_Loupe_Data\" />")
    set(LOUPE_WIX_FRAGMENT_OCR_SERVICE "")

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
        <File Id=\"fil${_loupe_icu_id}\" KeyPath=\"yes\" Source=\"\$(var.MyInstallDir)\\${_loupe_icu_name}\" />
      </Component>
")
        endforeach()
        if(_loupe_icu_index EQUAL 0)
            message(WARNING "No icu*.dll found under ${LOUPE_QT_ROOT}/bin")
        endif()
    endif()

    configure_file(LOUPE.sln.in LOUPE.sln)
    configure_file(LOUPE.wixproj.in LOUPE.wixproj)
    configure_file(Product.wxs.in Product.wxs)
    configure_file(createInstaller.ps1.in createInstaller.ps1)
    configure_file(explic.rtf explic.rtf)
endif()
