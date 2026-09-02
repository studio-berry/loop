# MIT License
#
# Copyright (c) 2018-2025 Jakub Melka and Contributors
#
# Deploy Sentry crashpad artifacts next to app binaries during development builds.

if(NOT LOOP_ENABLE_SENTRY)
    return()
endif()

if(NOT TARGET sentry_crashpad::crashpad_handler)
    return()
endif()

get_target_property(_loop_sentry_crashpad_handler sentry_crashpad::crashpad_handler IMPORTED_LOCATION_RELEASE)
if(NOT _loop_sentry_crashpad_handler)
    get_target_property(_loop_sentry_crashpad_handler sentry_crashpad::crashpad_handler IMPORTED_LOCATION)
endif()

if(TARGET sentry_crashpad::crashpad_wer)
    get_target_property(_loop_sentry_crashpad_wer sentry_crashpad::crashpad_wer IMPORTED_LOCATION_RELEASE)
    if(NOT _loop_sentry_crashpad_wer)
        get_target_property(_loop_sentry_crashpad_wer sentry_crashpad::crashpad_wer IMPORTED_LOCATION)
    endif()
endif()

function(loop_deploy_sentry_crashpad target)
    if(NOT LOOP_ENABLE_SENTRY)
        return()
    endif()

    if(NOT TARGET ${target})
        return()
    endif()

    if(_loop_sentry_crashpad_handler)
        if(WIN32)
            set(_loop_sentry_handler_name "crashpad_handler.exe")
        else()
            set(_loop_sentry_handler_name "crashpad_handler")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loop_sentry_crashpad_handler}"
                "$<TARGET_FILE_DIR:${target}>/${_loop_sentry_handler_name}"
            COMMENT "Copy ${_loop_sentry_handler_name} for Sentry")
    endif()

    if(WIN32 AND _loop_sentry_crashpad_wer)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loop_sentry_crashpad_wer}"
                "$<TARGET_FILE_DIR:${target}>/crashpad_wer.dll"
            COMMENT "Copy crashpad_wer.dll for Sentry")
    endif()
endfunction()
