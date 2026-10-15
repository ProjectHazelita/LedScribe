if(NOT DEFINED LEDSCRIBE OR NOT DEFINED TEST_DIR)
  message(FATAL_ERROR "LEDSCRIBE and TEST_DIR are required")
endif()

# Synthetic bitmap fonts keep these CLI tests independent of system fonts,
# vendor redistribution permissions and USB hardware.
file(MAKE_DIRECTORY "${TEST_DIR}/fonts")
string(REPEAT "0" 2816 ascii_font)
string(REPEAT "0" 529452 gbk_font)
file(WRITE "${TEST_DIR}/fonts/ASC11" "${ascii_font}")
file(WRITE "${TEST_DIR}/fonts/HZK11" "${gbk_font}")

function(expect_invalid option value category)
  execute_process(
    COMMAND "${LEDSCRIBE}" --dry-run --vendor-font-dir "${TEST_DIR}/fonts"
            "${option}" "${value}" "A"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    TIMEOUT 10
  )
  if(NOT result STREQUAL "1" OR NOT error MATCHES "${category}")
    message(FATAL_ERROR "${option} '${value}': expected ${category} error; got ${result}\n${output}${error}")
  endif()
  if(output MATCHES "encoding complete")
    message(FATAL_ERROR "Invalid arguments reached encoding: ${option} '${value}'")
  endif()
endfunction()

foreach(option IN ITEMS --speed --mode --blink --border)
  foreach(value IN ITEMS "" "," "1,,2" "1," ",1" "1,2,3,4,5,6,7,8,1")
    expect_invalid("${option}" "${value}" "list")
  endforeach()
endforeach()
expect_invalid(--speed "0" "speed")
expect_invalid(--speed "9" "speed")
expect_invalid(--speed "1,9" "speed")
expect_invalid(--mode "typo" "mode")
expect_invalid(--mode "9" "mode")
expect_invalid(--blink "0" "slot")
expect_invalid(--border "9" "slot")
expect_invalid(--font-size "5" "font-size")
expect_invalid(--threshold "-1" "threshold")
expect_invalid(--threshold "256" "threshold")
expect_invalid(--brightness "99" "brightness")

execute_process(
  COMMAND "${LEDSCRIBE}" --dry-run --preview --vendor-font-dir "${TEST_DIR}/fonts"
          --speed "1, 8" --mode "left,still" --blink all --border "1,2" "A" "B"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
  TIMEOUT 10
)
if(NOT result STREQUAL "0"
   OR NOT output MATCHES "\"A\" \\(8×11\\)"
   OR NOT output MATCHES "\"B\" \\(8×11\\)"
   OR NOT output MATCHES "encoding complete: 128 bytes, 2 CH546 reports")
  message(FATAL_ERROR "Valid multi-message dry-run failed: ${result}\n${output}${error}")
endif()

file(REMOVE_RECURSE "${TEST_DIR}/fonts")
