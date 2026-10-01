function(loop_stage_worker_runtime target)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DWORKER=$<TARGET_FILE:${target}>"
                "-DSEARCH_DIRS=${LOOP_QT_ROOT}/bin;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
                -P "${CMAKE_SOURCE_DIR}/PdfTool/write-worker-runtime.cmake"
            VERBATIM)
    endif()
endfunction()
