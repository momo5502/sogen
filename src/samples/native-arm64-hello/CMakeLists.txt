find_program(NATIVE_ARM64_MINGW_CC
  NAMES aarch64-w64-mingw32-gcc aarch64-w64-mingw32-clang
  HINTS "$ENV{HOME}/mingw/bin"
  REQUIRED
)
set(NATIVE_ARM64_SAMPLE "${CMAKE_CURRENT_BINARY_DIR}/native-arm64-hello.exe")
set(NATIVE_ARM64_SAMPLE "${NATIVE_ARM64_SAMPLE}" PARENT_SCOPE)
add_custom_command(OUTPUT "${NATIVE_ARM64_SAMPLE}"
  COMMAND "${NATIVE_ARM64_MINGW_CC}" -std=c11 -O2 -Wall -Wextra -Werror -nostdlib
          -Wl,--entry,mainCRTStartup -Wl,--subsystem,console
          "${CMAKE_CURRENT_SOURCE_DIR}/hello.c" -o "${NATIVE_ARM64_SAMPLE}" -lkernel32
  DEPENDS hello.c "${NATIVE_ARM64_MINGW_CC}"
  VERBATIM
)
add_custom_target(native-arm64-hello DEPENDS "${NATIVE_ARM64_SAMPLE}")
