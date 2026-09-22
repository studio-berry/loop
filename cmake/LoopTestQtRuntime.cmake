# Windows: a test process CTest starts inherits whatever PATH the caller's shell
# happened to have. Loop's Qt comes from LOOP_QT_ROOT (an aqt install), not from
# vcpkg, so VCPKG_APPLOCAL_DEPS copies no Qt DLL beside these executables and the
# loader stops on Qt6Core.dll/Qt6Quick.dll with a modal system error instead of a
# test failure (docs/CI.md, "Windows local test executables"). Record the Qt bin
# directory on every test this directory defines, so `ctest` resolves the Qt
# runtime from any shell, for any generator.
if(WIN32)
    # aqt's Qt kit declares only config-specific imported locations and no
    # IMPORTED_LOCATION_RELEASE, so query the configurations it does declare.
    get_target_property(_loop_qt_core_configs Qt6::Core IMPORTED_CONFIGURATIONS)
    set(_loop_qt_core_location "")
    foreach(_loop_qt_config IN LISTS _loop_qt_core_configs)
        if(_loop_qt_core_location)
            continue()
        endif()
        get_target_property(_loop_qt_core_location Qt6::Core IMPORTED_LOCATION_${_loop_qt_config})
    endforeach()
    if(NOT _loop_qt_core_location)
        get_target_property(_loop_qt_core_location Qt6::Core IMPORTED_LOCATION)
    endif()
    if(NOT _loop_qt_core_location)
        message(FATAL_ERROR
            "Qt6::Core exposes no imported DLL location; CTest runs cannot be given a Qt runtime path.")
    endif()

    get_filename_component(_loop_qt_bin_dir "${_loop_qt_core_location}" DIRECTORY)
    get_property(_loop_unit_tests DIRECTORY PROPERTY TESTS)

    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        foreach(_loop_test IN LISTS _loop_unit_tests)
            set_tests_properties(${_loop_test} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_loop_qt_bin_dir}")
        endforeach()
    else()
        # CMake < 3.22 has no ENVIRONMENT_MODIFICATION; hand the child a PATH that
        # starts with the Qt bin directory (semicolons escaped for the property).
        string(REPLACE ";" "\\;" _loop_inherited_path "$ENV{PATH}")
        foreach(_loop_test IN LISTS _loop_unit_tests)
            set_tests_properties(${_loop_test} PROPERTIES
                ENVIRONMENT "PATH=${_loop_qt_bin_dir}\\;${_loop_inherited_path}")
        endforeach()
    endif()
    message(STATUS "CTest Qt runtime path: ${_loop_qt_bin_dir}")
endif()
