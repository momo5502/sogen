file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

if(SOGEN_BUILD_STATIC)
  add_library(hvf-emulator STATIC ${SRC_FILES})
else()
  add_library(hvf-emulator SHARED ${SRC_FILES})
endif()

target_include_directories(hvf-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(hvf-emulator PUBLIC emulator)
target_link_libraries(hvf-emulator PRIVATE emulator-common "-framework Hypervisor")

sogen_strip_target(hvf-emulator)
