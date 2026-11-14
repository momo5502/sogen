add_library(native-arm64-emulator
  native_arm64_emulator.cpp
  native_arm64_emulator.hpp
  signal_entry.S
)
target_include_directories(native-arm64-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(native-arm64-emulator PUBLIC emulator)
sogen_strip_target(native-arm64-emulator)
