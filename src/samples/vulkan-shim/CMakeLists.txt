file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

# Export the Vulkan entry points under undecorated names (the real vulkan-1.dll does the same). Without
# this, __stdcall name decoration on 32-bit (WOW64) guests would hide them from GetProcAddress.
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  list(APPEND SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_shim.def)
endif()

add_library(vulkan-shim SHARED ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(vulkan-shim PRIVATE gpu-bridge-protocol vulkan-headers)
