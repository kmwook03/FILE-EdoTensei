file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}" "${TEST_ROOT}/out")
execute_process(
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/generate_fixtures.py" "${TEST_ROOT}"
    RESULT_VARIABLE fixture_result)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "fixture generation failed")
endif()
execute_process(
    COMMAND "${PROGRAM}" --mode raw --output "${TEST_ROOT}/out"
            --report "${TEST_ROOT}/report.jsonl" "${TEST_ROOT}/mixed.img"
    RESULT_VARIABLE carve_result)
if(NOT carve_result EQUAL 0)
    message(FATAL_ERROR "mixed fixture carving failed")
endif()
file(GLOB recovered "${TEST_ROOT}/out/recovered_*")
list(LENGTH recovered recovered_count)
if(NOT recovered_count EQUAL 3)
    message(FATAL_ERROR "expected 3 recovered files, got ${recovered_count}")
endif()
file(STRINGS "${TEST_ROOT}/report.jsonl" report_lines)
list(LENGTH report_lines report_count)
if(NOT report_count EQUAL 4)
    message(FATAL_ERROR "expected 3 candidate records and summary")
endif()
execute_process(
    COMMAND "${PROGRAM}" --mode raw --output "${TEST_ROOT}/out" "${TEST_ROOT}/mixed.img"
    RESULT_VARIABLE repeat_result)
if(NOT repeat_result EQUAL 0)
    message(FATAL_ERROR "repeat carving failed")
endif()
file(GLOB repeated "${TEST_ROOT}/out/recovered_*")
list(LENGTH repeated repeated_count)
if(NOT repeated_count EQUAL 6)
    message(FATAL_ERROR "repeat run overwrote output")
endif()
execute_process(
    COMMAND "${PROGRAM}" --mode raw --output "${TEST_ROOT}/boundary-out" "${TEST_ROOT}/boundary.img"
    RESULT_VARIABLE boundary_result)
if(NOT boundary_result EQUAL 0)
    message(FATAL_ERROR "boundary fixture carving failed")
endif()
file(GLOB boundary_files "${TEST_ROOT}/boundary-out/*.jpg")
list(LENGTH boundary_files boundary_count)
if(NOT boundary_count EQUAL 1)
    message(FATAL_ERROR "boundary-spanning signature was not recovered")
endif()
execute_process(
    COMMAND "${PROGRAM}" --mode raw "${TEST_ROOT}/empty.img"
    RESULT_VARIABLE empty_result)
if(NOT empty_result EQUAL 0)
    message(FATAL_ERROR "empty input should succeed")
endif()
execute_process(
    COMMAND "${PROGRAM}" --output "${TEST_ROOT}/hybrid-out" "${TEST_ROOT}/mixed.img"
    RESULT_VARIABLE legacy_result)
if(NOT legacy_result EQUAL 0)
    message(FATAL_ERROR "legacy/default hybrid invocation failed")
endif()
execute_process(
    COMMAND "${PROGRAM}" --mode raw --min-confidence 100
            --output "${TEST_ROOT}/threshold-out" "${TEST_ROOT}/mixed.img"
    RESULT_VARIABLE threshold_result)
if(NOT threshold_result EQUAL 0)
    message(FATAL_ERROR "confidence threshold run failed")
endif()
file(GLOB threshold_files "${TEST_ROOT}/threshold-out/recovered_*")
list(LENGTH threshold_files threshold_count)
if(NOT threshold_count EQUAL 2)
    message(FATAL_ERROR "confidence threshold did not filter weak PDF")
endif()
execute_process(
    COMMAND "${PROGRAM}" --mode raw --output "${TEST_ROOT}/truncated-out"
            --report "${TEST_ROOT}/truncated.jsonl" "${TEST_ROOT}/truncated.img"
    RESULT_VARIABLE truncated_result)
if(NOT truncated_result EQUAL 0)
    message(FATAL_ERROR "truncated candidate handling failed")
endif()
file(READ "${TEST_ROOT}/truncated.jsonl" truncated_report)
if(NOT truncated_report MATCHES "end_of_input_before_eoi")
    message(FATAL_ERROR "truncation reason missing from report")
endif()
execute_process(
    COMMAND "${PROGRAM}" --mode raw --output "${TEST_ROOT}/corrupt-out"
            --report "${TEST_ROOT}/corrupt.jsonl" "${TEST_ROOT}/corrupt.img"
    RESULT_VARIABLE corrupt_result)
if(NOT corrupt_result EQUAL 0)
    message(FATAL_ERROR "corrupt candidate scan should be nonfatal")
endif()
file(GLOB corrupt_files "${TEST_ROOT}/corrupt-out/recovered_*")
if(corrupt_files)
    message(FATAL_ERROR "corrupt PNG was recovered")
endif()
execute_process(COMMAND "${PROGRAM}" --mode nonsense "${TEST_ROOT}/empty.img"
                RESULT_VARIABLE invalid_mode OUTPUT_QUIET ERROR_QUIET)
if(invalid_mode EQUAL 0)
    message(FATAL_ERROR "invalid mode was accepted")
endif()
execute_process(COMMAND "${PROGRAM}" --min-confidence 101 "${TEST_ROOT}/empty.img"
                RESULT_VARIABLE invalid_confidence OUTPUT_QUIET ERROR_QUIET)
if(invalid_confidence EQUAL 0)
    message(FATAL_ERROR "invalid confidence was accepted")
endif()
execute_process(COMMAND "${PROGRAM}" --mode ntfs "${TEST_ROOT}/empty.img"
                RESULT_VARIABLE invalid_ntfs OUTPUT_QUIET ERROR_QUIET)
if(invalid_ntfs EQUAL 0)
    message(FATAL_ERROR "explicit NTFS mode should reject non-NTFS input")
endif()
execute_process(COMMAND "${PROGRAM}" "${TEST_ROOT}/missing.img"
                RESULT_VARIABLE missing_result OUTPUT_QUIET ERROR_QUIET)
if(missing_result EQUAL 0)
    message(FATAL_ERROR "missing input was accepted")
endif()
execute_process(COMMAND "${PROGRAM}" --mode raw --output "${TEST_ROOT}/not-a-directory"
                        "${TEST_ROOT}/mixed.img"
                RESULT_VARIABLE output_failure OUTPUT_QUIET ERROR_QUIET)
if(output_failure EQUAL 0)
    message(FATAL_ERROR "unwritable output destination was accepted")
endif()
execute_process(COMMAND "${PROGRAM}" --mode raw --output "${TEST_ROOT}/embedded-out"
                        "${TEST_ROOT}/embedded.img"
                RESULT_VARIABLE embedded_result)
if(NOT embedded_result EQUAL 0)
    message(FATAL_ERROR "embedded JPEG/PDF fixture failed")
endif()
file(GLOB embedded_files "${TEST_ROOT}/embedded-out/recovered_*")
list(LENGTH embedded_files embedded_count)
if(NOT embedded_count EQUAL 2)
    message(FATAL_ERROR "embedded JPEG should not invalidate PDF")
endif()
execute_process(COMMAND "${PROGRAM}" --mode raw --output "${TEST_ROOT}/adjacent-out"
                        "${TEST_ROOT}/adjacent.img"
                RESULT_VARIABLE adjacent_result)
if(NOT adjacent_result EQUAL 0)
    message(FATAL_ERROR "adjacent fixture failed")
endif()
file(GLOB adjacent_files "${TEST_ROOT}/adjacent-out/recovered_*")
list(LENGTH adjacent_files adjacent_count)
if(NOT adjacent_count EQUAL 2)
    message(FATAL_ERROR "adjacent files were not recovered independently")
endif()
execute_process(COMMAND "${PROGRAM}" --mode ntfs --output "${TEST_ROOT}/ntfs-out"
                        --report "${TEST_ROOT}/ntfs.jsonl" "${TEST_ROOT}/resident.ntfs"
                RESULT_VARIABLE ntfs_result)
if(NOT ntfs_result EQUAL 0)
    message(FATAL_ERROR "resident NTFS recovery failed")
endif()
file(GLOB ntfs_files "${TEST_ROOT}/ntfs-out/*.jpg")
list(LENGTH ntfs_files ntfs_count)
if(NOT ntfs_count EQUAL 1)
    message(FATAL_ERROR "resident NTFS file was not recovered")
endif()
file(READ "${TEST_ROOT}/ntfs.jsonl" ntfs_report)
if(NOT ntfs_report MATCHES "ntfs_metadata" OR NOT ntfs_report MATCHES "lost.jpg")
    message(FATAL_ERROR "NTFS method or source filename missing from report")
endif()
