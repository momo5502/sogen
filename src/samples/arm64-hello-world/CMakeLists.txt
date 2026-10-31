add_executable(arm64-hello-world hello_world.c)
target_link_libraries(arm64-hello-world PRIVATE kernel32)
target_compile_options(arm64-hello-world PRIVATE /GS-)
target_link_options(arm64-hello-world PRIVATE /ENTRY:mainCRTStartup /NODEFAULTLIB /INCREMENTAL:NO)
set_target_properties(arm64-hello-world PROPERTIES OUTPUT_NAME hello_world)

add_executable(arm64-hello-crt hello_crt.c)
set_target_properties(arm64-hello-crt PROPERTIES OUTPUT_NAME hello_crt)

add_executable(arm64-crt-static crt_static.c)
set_target_properties(arm64-crt-static PROPERTIES OUTPUT_NAME crt_static MSVC_RUNTIME_LIBRARY MultiThreaded)

enable_language(ASM_MARMASM)
add_executable(arm64-instrumentation-callback instrumentation_callback.c instrumentation_callback.asm)
target_link_libraries(arm64-instrumentation-callback PRIVATE advapi32)
set_target_properties(arm64-instrumentation-callback PROPERTIES OUTPUT_NAME instrumentation_callback)

foreach(sample arm64-hello-world arm64-hello-crt arm64-crt-static arm64-instrumentation-callback)
  sogen_target_skip_clang_tidy(${sample})
endforeach()

if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME arm64-native-hello-world COMMAND arm64-hello-world)
  set_tests_properties(arm64-native-hello-world PROPERTIES TIMEOUT 30)
endif()
