set(MACOS_HELLO_SAMPLE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/macos-hello-sample")

add_custom_command(
  OUTPUT "${MACOS_HELLO_SAMPLE}"
  COMMAND "${CMAKE_C_COMPILER}" -arch x86_64 -static -nostdlib -e _start "${CMAKE_CURRENT_SOURCE_DIR}/hello.s" -o "${MACOS_HELLO_SAMPLE}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/hello.s"
  COMMENT "Building x86-64 Mach-O sample"
  VERBATIM
)

add_custom_target(macos-hello-sample ALL DEPENDS "${MACOS_HELLO_SAMPLE}")
