add_library(seccomp-emulator
  seccomp_emulator.cpp
  seccomp_emulator.hpp
  seccomp_single_step.cpp
  seccomp_single_step.hpp
  seccomp_state.hpp
  signal_entry.S
)
target_include_directories(seccomp-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(seccomp-emulator PUBLIC emulator)
sogen_strip_target(seccomp-emulator)
