if(SOGEN_BUILD_STATIC)
  add_library(whp-arm64-emulator STATIC whp_arm64_emulator.cpp whp_arm64_emulator.hpp)
else()
  add_library(whp-arm64-emulator SHARED whp_arm64_emulator.cpp whp_arm64_emulator.hpp)
endif()

target_include_directories(whp-arm64-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(whp-arm64-emulator PUBLIC emulator)
target_link_libraries(whp-arm64-emulator PRIVATE WinHvPlatform)
sogen_strip_target(whp-arm64-emulator)

if(SOGEN_BUILD_TOOLS)
  add_subdirectory(tests)
endif()
