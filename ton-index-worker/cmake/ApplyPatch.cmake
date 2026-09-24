if(NOT DEFINED PATCH_FILE)
  message(FATAL_ERROR "PATCH_FILE is not set")
endif()
if(NOT DEFINED PATCH_WORKING_DIRECTORY)
  message(FATAL_ERROR "PATCH_WORKING_DIRECTORY is not set")
endif()
if(NOT PATCH_EXECUTABLE)
  message(FATAL_ERROR "PATCH_EXECUTABLE is not set")
endif()

if(NOT EXISTS "${PATCH_FILE}")
  message(FATAL_ERROR "Patch file does not exist: ${PATCH_FILE}")
endif()
if(NOT EXISTS "${PATCH_WORKING_DIRECTORY}")
  message(FATAL_ERROR "Patch working directory does not exist: ${PATCH_WORKING_DIRECTORY}")
endif()

execute_process(
  COMMAND "${PATCH_EXECUTABLE}" -p1 -d "${PATCH_WORKING_DIRECTORY}" --batch --forward
    --fuzz=0 --no-backup-if-mismatch -r - --dry-run -i "${PATCH_FILE}"
  RESULT_VARIABLE patch_can_apply
  OUTPUT_VARIABLE patch_check_output
  ERROR_VARIABLE patch_check_error
)

if(patch_can_apply STREQUAL "0")
  execute_process(
    COMMAND "${PATCH_EXECUTABLE}" -p1 -d "${PATCH_WORKING_DIRECTORY}" --batch --forward
      --fuzz=0 --no-backup-if-mismatch -r - -i "${PATCH_FILE}"
    RESULT_VARIABLE patch_apply_result
    OUTPUT_VARIABLE patch_apply_output
    ERROR_VARIABLE patch_apply_error
  )
  if(NOT patch_apply_result STREQUAL "0")
    message(FATAL_ERROR
      "Failed to apply patch ${PATCH_FILE} (patch exit ${patch_apply_result}):\n"
      "${patch_apply_output}\n"
      "${patch_apply_error}"
    )
  endif()
  message(STATUS "Applied patch: ${PATCH_FILE}")
  return()
endif()
if(NOT patch_can_apply STREQUAL "1")
  message(FATAL_ERROR
    "Patch dry run failed for ${PATCH_FILE} (patch exit ${patch_can_apply}):\n"
    "${patch_check_output}\n${patch_check_error}"
  )
endif()

execute_process(
  COMMAND "${PATCH_EXECUTABLE}" -p1 -d "${PATCH_WORKING_DIRECTORY}" --batch --reverse
    --fuzz=0 --no-backup-if-mismatch -r - --dry-run -i "${PATCH_FILE}"
  RESULT_VARIABLE patch_already_applied
  OUTPUT_VARIABLE patch_reverse_check_output
  ERROR_VARIABLE patch_reverse_check_error
)

if(patch_already_applied STREQUAL "0")
  message(STATUS "Patch already applied: ${PATCH_FILE}")
  return()
endif()

message(FATAL_ERROR
  "Patch cannot be applied cleanly and is not already applied: ${PATCH_FILE}\n"
  "patch --forward --dry-run (exit ${patch_can_apply}):\n${patch_check_output}\n${patch_check_error}\n"
  "patch --reverse --dry-run (exit ${patch_already_applied}):\n${patch_reverse_check_output}\n${patch_reverse_check_error}"
)
