add_library(vulkan-bridge-marshal INTERFACE)

target_sources(vulkan-bridge-marshal INTERFACE
  vk_bridge_serial.hpp
  vk_bridge_marshal.generated.hpp
)

target_include_directories(vulkan-bridge-marshal INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(vulkan-bridge-marshal INTERFACE
  vulkan-headers
  gpu-bridge-protocol
)

# Offline regeneration of the marshalling code from vk.xml. Not part of the normal build (the
# generated header is checked in); run explicitly after changing the allowlist or updating the
# Vulkan-Headers submodule.
add_custom_target(vulkan-bridge-generate
  COMMAND "${PYTHON3_EXE}"
          "${PROJECT_SOURCE_DIR}/tools/vulkan-bridge-generator/generate.py"
          --vk-xml "${PROJECT_SOURCE_DIR}/deps/Vulkan-Headers/registry/vk.xml"
          --output "${CMAKE_CURRENT_LIST_DIR}/vk_bridge_marshal.generated.hpp"
  COMMENT "Regenerating Vulkan bridge marshalling from vk.xml"
  VERBATIM
)
