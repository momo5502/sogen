file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

if(SOGEN_USE_SYSTEM_SDL3 AND NOT EMSCRIPTEN AND NOT TARGET SDL3::SDL3)
  find_package(SDL3 CONFIG REQUIRED)
endif()

if(NOT TARGET SDL3::SDL3)
  list(REMOVE_ITEM SRC_FILES "${CMAKE_CURRENT_LIST_DIR}/ui_backends/sdl_ui_backend.cpp")
  if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "The SDL3 submodule (deps/SDL) is required for the host UI backend on this platform")
  endif()
endif()

add_library(windows-emulator ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

if(NOT SOGEN_ENABLE_CLANG_TIDY)
  target_precompile_headers(windows-emulator PRIVATE std_include.hpp)
endif()

target_link_libraries(windows-emulator PUBLIC emulator minidump)
target_link_libraries(windows-emulator PRIVATE gpu-bridge-protocol vulkan-headers steam-bridge-protocol)

# When the Steamworks SDK was provided, link the real Steam host backend (owns the SDK headers + the
# real steamclient64.dll connection behind a clean C ABI) and let the device use it.
if(TARGET steam-host-backend)
  target_link_libraries(windows-emulator PRIVATE steam-host-backend)
  target_compile_definitions(windows-emulator PRIVATE SOGEN_STEAM_REAL_BACKEND=1)
endif()

if(TARGET SDL3::SDL3)
  target_link_libraries(windows-emulator PRIVATE SDL3::SDL3)
  target_compile_definitions(windows-emulator PRIVATE SOGEN_HAS_SDL3=1)
endif()

target_include_directories(windows-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

sogen_strip_target(windows-emulator)
