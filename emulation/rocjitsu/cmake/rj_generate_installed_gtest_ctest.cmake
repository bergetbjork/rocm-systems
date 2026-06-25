# Build-time helper for installed GoogleTest discovery.
#
# This script is run with `cmake -P` after TEST_EXECUTABLE has been built. It
# asks GoogleTest for the concrete test list, including parameterized TEST_P
# expansions, then writes a CTest fragment that runs the installed executable
# once per discovered test case.
#
# Required variables:
#   TEST_EXECUTABLE      Build-tree executable used for --gtest_list_tests.
#   INSTALLED_EXECUTABLE Path CTest should run from the installed test dir.
#   OUTPUT_FILE          Generated CTest fragment path.
function(rj_generate_installed_gtest_ctest)
    set(oneValueArgs TEST_EXECUTABLE INSTALLED_EXECUTABLE OUTPUT_FILE)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})

    if(NOT ARG_TEST_EXECUTABLE)
        message(FATAL_ERROR "TEST_EXECUTABLE is required")
    endif()
    if(NOT ARG_INSTALLED_EXECUTABLE)
        message(FATAL_ERROR "INSTALLED_EXECUTABLE is required")
    endif()
    if(NOT ARG_OUTPUT_FILE)
        message(FATAL_ERROR "OUTPUT_FILE is required")
    endif()

    execute_process(
        COMMAND
            "${ARG_TEST_EXECUTABLE}" --gtest_list_tests
            "--gtest_filter=-*Benchmark*"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _tests
        ERROR_VARIABLE _errors
    )
    if(NOT _result EQUAL 0)
        message(
            FATAL_ERROR
            "Failed to list tests from ${ARG_TEST_EXECUTABLE}: ${_errors}"
        )
    endif()

    file(WRITE "${ARG_OUTPUT_FILE}" "# Installed GoogleTest CTest tests\n")
    set(_suite)
    string(REPLACE "\n" ";" _lines "${_tests}")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^([^# ]+)\\.$")
            set(_suite "${CMAKE_MATCH_1}")
        elseif(_line MATCHES "^  ([^# ]+)")
            if(NOT _suite)
                message(FATAL_ERROR "Found gtest case without suite: ${_line}")
            endif()
            set(_case_name "${CMAKE_MATCH_1}")
            set(_pretty_case_name "${_case_name}")
            if(
                _case_name MATCHES "/[0-9]+$"
                AND _line MATCHES "# GetParam\\(\\) = \"?([A-Za-z0-9_]+)\"?$"
            )
                string(
                    REGEX REPLACE
                    "/[0-9]+$"
                    "/${CMAKE_MATCH_1}"
                    _pretty_case_name
                    "${_case_name}"
                )
            endif()
            set(_test_name "${_suite}.${_pretty_case_name}")
            set(_test_filter "${_suite}.${_case_name}")
            string(REPLACE "\\" "\\\\" _escaped_name "${_test_name}")
            string(REPLACE "\"" "\\\"" _escaped_name "${_escaped_name}")
            string(REPLACE "\\" "\\\\" _escaped_filter "${_test_filter}")
            string(REPLACE "\"" "\\\"" _escaped_filter "${_escaped_filter}")
            file(
                APPEND "${ARG_OUTPUT_FILE}"
                "add_test(\"${_escaped_name}\" \"${ARG_INSTALLED_EXECUTABLE}\" \"--gtest_filter=${_escaped_filter}\")\n\n"
            )
        endif()
    endforeach()
endfunction()

rj_generate_installed_gtest_ctest(
    TEST_EXECUTABLE "${TEST_EXECUTABLE}"
    INSTALLED_EXECUTABLE "${INSTALLED_EXECUTABLE}"
    OUTPUT_FILE "${OUTPUT_FILE}"
)
