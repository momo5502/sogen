# Builds the icicle bridge in two phases so we never have to guess whether a
# failure was a transient network blip or a genuine build error:
#
#   1. `cargo fetch` downloads every dependency (git + crates.io). This phase is
#      pure network, so ANY failure is a fetch failure and is retried to ride out
#      the frequent GitHub/crates.io flakiness in CI (curl 56, OpenSSL
#      "unexpected eof while reading", ...).
#   2. `cargo build --offline` compiles against the already-fetched deps with no
#      network access and no retries — a failure here is a real build error and
#      fails fast.
#
# Invoked via `cmake -P` from CMakeLists.txt; per-request fetch robustness lives
# in .cargo/config.toml.

if(NOT DEFINED CARGO_RETRIES)
  set(CARGO_RETRIES 5)
endif()

if(NOT DEFINED CARGO_RETRY_DELAY)
  set(CARGO_RETRY_DELAY 15)
endif()

set(target_option)
if(CARGO_TRIPLE)
  set(target_option --target=${CARGO_TRIPLE})
endif()

set(cargo_env ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}" --)

# Phase 1: fetch dependencies, retrying transient network failures.
set(attempt 1)
while(TRUE)
  message(STATUS "icicle: cargo fetch (attempt ${attempt}/${CARGO_RETRIES})")

  execute_process(
    COMMAND ${cargo_env} cargo fetch ${target_option}
    WORKING_DIRECTORY ${CARGO_MANIFEST_DIR}
    RESULT_VARIABLE fetch_result
  )

  if(fetch_result EQUAL 0)
    break()
  endif()

  if(attempt GREATER_EQUAL CARGO_RETRIES)
    message(FATAL_ERROR "icicle: cargo fetch failed after ${CARGO_RETRIES} attempts (last exit code: ${fetch_result})")
  endif()

  message(WARNING "icicle: cargo fetch failed (exit ${fetch_result}); retrying in ${CARGO_RETRY_DELAY}s")
  execute_process(COMMAND ${CMAKE_COMMAND} -E sleep ${CARGO_RETRY_DELAY})
  math(EXPR attempt "${attempt} + 1")
endwhile()

# Phase 2: build offline against the fetched deps. No retries: a failure here is a
# genuine build error and should surface immediately.
message(STATUS "icicle: cargo build (offline)")

execute_process(
  COMMAND ${cargo_env} cargo build --offline --lib --profile ${CARGO_PROFILE} ${target_option}
  WORKING_DIRECTORY ${CARGO_MANIFEST_DIR}
  RESULT_VARIABLE build_result
)

if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "icicle: cargo build failed (exit ${build_result})")
endif()
