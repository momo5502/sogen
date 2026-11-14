find_program(NATIVE_ARM64_MINGW_CC
  NAMES aarch64-w64-mingw32-gcc aarch64-w64-mingw32-clang
  HINTS "$ENV{HOME}/mingw/bin"
  REQUIRED
)
function(add_native_arm64_sample target source output_variable)
  set(sample "${CMAKE_CURRENT_BINARY_DIR}/${target}.exe")
  add_custom_command(OUTPUT "${sample}"
    COMMAND "${NATIVE_ARM64_MINGW_CC}" -std=c11 -O2 -Wall -Wextra -Werror -nostdlib
            -Wl,--entry,mainCRTStartup -Wl,--subsystem,console
            "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${sample}" -lkernel32
    DEPENDS "${source}" "${NATIVE_ARM64_MINGW_CC}"
    VERBATIM
  )
  add_custom_target(${target} DEPENDS "${sample}")
  set(${output_variable} "${sample}" PARENT_SCOPE)
endfunction()

add_native_arm64_sample(native-arm64-hello hello.c NATIVE_ARM64_SAMPLE)
add_native_arm64_sample(native-arm64-illegal-instruction illegal_instruction.c NATIVE_ARM64_ILLEGAL_INSTRUCTION_SAMPLE)
set(NATIVE_ARM64_SAMPLE "${NATIVE_ARM64_SAMPLE}" PARENT_SCOPE)
set(NATIVE_ARM64_ILLEGAL_INSTRUCTION_SAMPLE "${NATIVE_ARM64_ILLEGAL_INSTRUCTION_SAMPLE}" PARENT_SCOPE)
