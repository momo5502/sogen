add_library(gpu-bridge-protocol INTERFACE)

target_sources(gpu-bridge-protocol INTERFACE
  gpu_bridge_protocol.hpp
  vk_feature_chain.hpp
)

target_include_directories(gpu-bridge-protocol INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
