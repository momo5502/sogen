# Runs `cargo build` for the icicle bridge, retrying a few times to ride out the
# frequent GitHub/crates.io network flakiness in CI (curl 56 / OpenSSL
# "unexpected eof while reading"). A single transient fetch failure would
# otherwise fail the whole build. Invoked via `cmake -P` from CMakeLists.txt;
# per-request robustness lives in .cargo/config.toml.

if(NOT DEFINED CARGO_RETRIES)
  set(CARGO_RETRIES 5)
endif()

if(NOT DEFINED CARGO_RETRY_DELAY)
  set(CARGO_RETRY_DELAY 15)
endif()

set(cargo_args build --lib --profile ${CARGO_PROFILE})
if(CARGO_TRIPLE)
  list(APPEND cargo_args --target=${CARGO_TRIPLE})
endif()

set(attempt 1)
while(TRUE)
  message(STATUS "icicle: cargo build (attempt ${attempt}/${CARGO_RETRIES})")

  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}" -- cargo ${cargo_args}
    WORKING_DIRECTORY ${CARGO_MANIFEST_DIR}
    RESULT_VARIABLE result
  )

  if(result EQUAL 0)
    break()
  endif()

  if(attempt GREATER_EQUAL CARGO_RETRIES)
    message(FATAL_ERROR "icicle: cargo build failed after ${CARGO_RETRIES} attempts (last exit code: ${result})")
  endif()

  message(WARNING "icicle: cargo build failed (exit ${result}); retrying in ${CARGO_RETRY_DELAY}s")
  execute_process(COMMAND ${CMAKE_COMMAND} -E sleep ${CARGO_RETRY_DELAY})
  math(EXPR attempt "${attempt} + 1")
endwhile()
