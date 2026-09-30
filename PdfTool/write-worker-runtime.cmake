cmake_policy(VERSION 3.16)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${WORKER}"
    DIRECTORIES ${SEARCH_DIRS}
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    CONFLICTING_DEPENDENCIES_PREFIX conflicts
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*")
if(unresolved OR conflicts_FILENAMES)
    message(FATAL_ERROR "Worker dependency closure is unresolved or ambiguous: ${unresolved};${conflicts_FILENAMES}")
endif()
get_filename_component(directory "${WORKER}" DIRECTORY)
file(MAKE_DIRECTORY "${directory}/worker-runtime")
set(manifest "")
foreach(dependency IN LISTS dependencies)
    get_filename_component(name "${dependency}" NAME)
    file(COPY "${dependency}" DESTINATION "${directory}/worker-runtime")
    string(APPEND manifest "worker-runtime/${name}\n")
endforeach()
file(WRITE "${WORKER}.runtime" "${manifest}")
