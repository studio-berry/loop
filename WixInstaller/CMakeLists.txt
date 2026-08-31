# MIT License
#
# The Windows installer contains only the current LoupeEditor and PdfTool
# product boundary. Retired Widgets products and plugins are deliberately not
# represented here, even when a developer build happens to contain old files.

if(WIN32 AND MSVC AND LOUPE_INSTALL_PREPARE_WIX_INSTALLER)
    if(LOUPE_LOUPE_DISTRIBUTION)
        # Release packaging is the only supported installer boundary.
        set(LOUPE_WIX_QT_WIDGETS_COMPONENT "")
    endif()
    if(LOUPE_INSTALL_TO_USR)
        file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}/usr/bin" WIX_INSTALL_PREFIX)
    else()
        file(TO_NATIVE_PATH "${CMAKE_INSTALL_PREFIX}/bin" WIX_INSTALL_PREFIX)
    endif()
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}/WixInstaller" LOUPE_WIX_DIR)

    set(LOUPE_WIX_FRAMEWORK_WIDGETS_LIBS "")
    set(LOUPE_WIX_EDITOR_PLUGINS "")
    set(LOUPE_WIX_EDITOR_PLUGIN_FEATURE "<ComponentGroupRef Id=\"LOUPE_Loupe_Data\" />")
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
