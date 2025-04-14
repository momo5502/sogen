include(ExternalProject)

set(ICICLE_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo-build)
set(ICICLE_ARTIFACT_DIR ${ICICLE_BUILD_DIR}/$<IF:$<CONFIG:Debug>,debug,release>)

set(ICICLE_RUST_PROJECT_NAME "icicle")
set(ICICLE_RUST_LIBNAME "lib${ICICLE_RUST_PROJECT_NAME}.a")

if(MSVC)
    set(ICICLE_RUST_LIBNAME "${ICICLE_RUST_PROJECT_NAME}.lib")
endif()

set(CARGO_TRIPLE)
set(CARGO_OPTIONS)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
  set(CARGO_TRIPLE "i686-pc-windows-msvc")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(CARGO_TRIPLE "aarch64-apple-ios")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
  if(CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    set(CARGO_TRIPLE "aarch64-linux-android")
  else()
    set(CARGO_TRIPLE "${CMAKE_ANDROID_ARCH_ABI}-linux-android")
  endif()
endif()

if(CARGO_TRIPLE)
  set(CARGO_OPTIONS ${CARGO_OPTIONS} "--target=${CARGO_TRIPLE}")
  set(ICICLE_ARTIFACT_DIR ${ICICLE_BUILD_DIR}/${CARGO_TRIPLE}/$<IF:$<CONFIG:Debug>,debug,release>)
endif()


set(ICICLE_RUST_LIB ${ICICLE_ARTIFACT_DIR}/${ICICLE_RUST_LIBNAME})

ExternalProject_Add(
    icicle-rust-project
    PREFIX ${CMAKE_CURRENT_BINARY_DIR}
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    BINARY_DIR ${CMAKE_CURRENT_LIST_DIR}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${ICICLE_BUILD_DIR}" cargo build ${CARGO_OPTIONS} --lib --profile $<IF:$<CONFIG:Debug>,dev,release>
    INSTALL_COMMAND ""
    USES_TERMINAL_CONFIGURE 1
    USES_TERMINAL_BUILD 1
    BUILD_ALWAYS 1
    BUILD_BYPRODUCTS ${ICICLE_RUST_LIB}
)

add_custom_command(
    TARGET icicle-rust-project POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_LIST_DIR}/data" "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    COMMENT "Copying Ghidra Processor Specification"
)

add_library(icicle-bridge INTERFACE)
add_dependencies(icicle-bridge icicle-rust-project)
target_link_libraries(icicle-bridge INTERFACE ${ICICLE_RUST_LIB})

if(MSVC)
  target_link_libraries(icicle-bridge INTERFACE
    ws2_32.lib
    Userenv.lib
    ntdll.lib
    Bcrypt.lib
  )
endif()