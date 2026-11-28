if(NOT SOGEN_ENABLE_SDL3 OR EMSCRIPTEN OR ANDROID)
  return()
endif()
if(NOT TARGET SDL3::SDL3)
  find_package(SDL3 CONFIG REQUIRED)
endif()

add_executable(ui-cursor-diagnostic
  main.cpp
  ../windows-emulator/keyboard_layout.cpp
  ../windows-emulator/ui_backends/sdl_ui_backend.cpp
)
target_link_libraries(ui-cursor-diagnostic PRIVATE emulator-common vulkan-headers SDL3::SDL3)
sogen_targets_set_folder("tools" ui-cursor-diagnostic)
sogen_strip_target(ui-cursor-diagnostic)

add_test(NAME ui-system-cursors COMMAND ui-cursor-diagnostic --check)
set_tests_properties(ui-system-cursors PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)
add_test(NAME ui-vulkan-windows COMMAND ui-cursor-diagnostic --check-vulkan-windows)
set_tests_properties(ui-vulkan-windows PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)
add_test(NAME ui-window-renderers COMMAND ui-cursor-diagnostic --check-window-renderers)
set_tests_properties(ui-window-renderers PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)
