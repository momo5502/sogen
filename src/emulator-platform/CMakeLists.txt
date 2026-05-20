file(GLOB PLATFORM_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/platform/*.hpp"
)

set(PLATFORM_FILES
  ${PLATFORM_FILES}
  "${CMAKE_CURRENT_LIST_DIR}/segment_utils.hpp"
)

list(SORT PLATFORM_FILES)

add_library(emulator-platform INTERFACE)

target_sources(emulator-platform INTERFACE ${PLATFORM_FILES})

target_include_directories(emulator-platform INTERFACE
  "${CMAKE_CURRENT_LIST_DIR}"
  "${CMAKE_SOURCE_DIR}/src"
)
