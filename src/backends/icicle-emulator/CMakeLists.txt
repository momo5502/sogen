add_subdirectory(icicle-bridge)

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

if(MOMO_BUILD_AS_LIBRARY)
	add_library(icicle-emulator STATIC ${SRC_FILES})
else()
    add_library(icicle-emulator SHARED ${SRC_FILES})
endif()

target_include_directories(icicle-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(icicle-emulator PUBLIC emulator)
target_link_libraries(icicle-emulator PRIVATE emulator-common icicle-bridge)
