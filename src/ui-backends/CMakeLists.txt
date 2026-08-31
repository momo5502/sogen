file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

if(SOGEN_USE_SYSTEM_SDL3 AND NOT EMSCRIPTEN AND NOT TARGET SDL3::SDL3)
  find_package(SDL3 CONFIG REQUIRED)
endif()

if(NOT TARGET SDL3::SDL3)
  list(REMOVE_ITEM SRC_FILES "${CMAKE_CURRENT_LIST_DIR}/sdl_ui_backend.cpp")
  if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "The SDL3 submodule (deps/SDL) is required for the host UI backend on this platform")
  endif()
endif()

add_library(ui-backends ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

if(NOT SOGEN_ENABLE_CLANG_TIDY)
  target_precompile_headers(ui-backends PRIVATE std_include.hpp)
endif()

target_link_libraries(ui-backends PUBLIC emulator-platform emulator-common)

if(TARGET SDL3::SDL3)
  target_link_libraries(ui-backends PRIVATE SDL3::SDL3)
  target_compile_definitions(ui-backends PRIVATE SOGEN_HAS_SDL3=1)
endif()

target_include_directories(ui-backends INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

sogen_strip_target(ui-backends)
