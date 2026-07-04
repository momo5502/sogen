# Runs `cargo build` for the icicle bridge, retrying transient GitHub/crates.io
# network failures (curl 56, OpenSSL "unexpected eof while reading", registry/git
# fetch errors) that frequently break CI. Genuine build errors are NOT retried:
# only failures whose output matches a known network-error signature trigger a
# retry, so a real compile error fails fast. Invoked via `cmake -P` from
# CMakeLists.txt; per-request robustness lives in .cargo/config.toml.

if(NOT DEFINED CARGO_RETRIES)
  set(CARGO_RETRIES 5)
endif()

if(NOT DEFINED CARGO_RETRY_DELAY)
  set(CARGO_RETRY_DELAY 15)
endif()

# Lowercased substrings that identify a transient network / dependency-fetch
# failure (matched against cargo's combined output). Kept deliberately specific
# so genuine build errors don't accidentally look retryable.
set(network_error_pattern
    "failed to get|failed to download|failed to load source|unable to update registry|spurious network error|unexpected eof|ssl|curl|timed out|failed to fetch|network failure|connection reset|connection closed|error sending request|could not download|failed to clone|download of")

set(cargo_args build --lib --profile ${CARGO_PROFILE})
if(CARGO_TRIPLE)
  list(APPEND cargo_args --target=${CARGO_TRIPLE})
endif()

set(attempt 1)
while(TRUE)
  message(STATUS "icicle: cargo build (attempt ${attempt}/${CARGO_RETRIES})")

  # Capture the output so we can classify the failure, while still streaming it live.
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}" -- cargo ${cargo_args}
    WORKING_DIRECTORY ${CARGO_MANIFEST_DIR}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout_text
    ERROR_VARIABLE stderr_text
    ECHO_OUTPUT_VARIABLE
    ECHO_ERROR_VARIABLE
  )

  if(result EQUAL 0)
    break()
  endif()

  string(TOLOWER "${stdout_text}${stderr_text}" combined_lower)
  string(REGEX MATCH "${network_error_pattern}" network_error "${combined_lower}")

  if(NOT network_error)
    message(FATAL_ERROR "icicle: cargo build failed (exit ${result}); output is not a transient network error, not retrying")
  endif()

  if(attempt GREATER_EQUAL CARGO_RETRIES)
    message(FATAL_ERROR "icicle: cargo build failed after ${CARGO_RETRIES} attempts due to network errors (last exit code: ${result})")
  endif()

  message(WARNING "icicle: cargo build hit a transient network error (exit ${result}); retrying in ${CARGO_RETRY_DELAY}s")
  execute_process(COMMAND ${CMAKE_COMMAND} -E sleep ${CARGO_RETRY_DELAY})
  math(EXPR attempt "${attempt} + 1")
endwhile()
