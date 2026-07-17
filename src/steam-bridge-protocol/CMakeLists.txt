add_library(steam-bridge-protocol INTERFACE)

target_sources(steam-bridge-protocol INTERFACE
  steam_bridge_protocol.hpp
)

target_include_directories(steam-bridge-protocol INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
