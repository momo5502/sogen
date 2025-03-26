include(ExternalProject)

set(ICICLE_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo-build)
set(ICICLE_ARTIFACT_DIR ${ICICLE_BUILD_DIR}/$<IF:$<CONFIG:Debug>,debug,release>)

set(ICICLE_RUST_PROJECT_NAME "icicle")
set(ICICLE_RUST_LIB ${ICICLE_ARTIFACT_DIR}/lib${ICICLE_RUST_PROJECT_NAME}.a)

if(MSVC)
    set(ICICLE_RUST_LIB ${ICICLE_ARTIFACT_DIR}/${ICICLE_RUST_PROJECT_NAME}.lib)
endif()

set(CARGO_OPTIONS "")

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(CARGO_OPTIONS "--target=aarch64-apple-ios")
endif()

ExternalProject_Add(
    icicle-rust-project
    PREFIX ${CMAKE_CURRENT_BINARY_DIR}
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    BINARY_DIR ${CMAKE_CURRENT_LIST_DIR}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${ICICLE_BUILD_DIR}" cargo build ${CARGO_OPTIONS} --all-targets $<IF:$<CONFIG:Debug>,--lib,--release>
    INSTALL_COMMAND ""
    USES_TERMINAL_CONFIGURE 1
    USES_TERMINAL_BUILD 1
    BUILD_ALWAYS 1
    BUILD_BYPRODUCTS ${ICICLE_RUST_LIB}
)

add_library(icicle INTERFACE)
add_dependencies(icicle icicle-rust-project)
target_link_libraries(icicle INTERFACE ${ICICLE_RUST_LIB})

if(MSVC)
  target_link_libraries(icicle INTERFACE
    ws2_32.lib
    Userenv.lib
    ntdll.lib
    Bcrypt.lib
  )
endif()