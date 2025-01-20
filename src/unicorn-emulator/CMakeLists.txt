file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

if(MOMO_BUILD_AS_LIBRARY)
	add_library(unicorn-emulator STATIC ${SRC_FILES})
else()
    add_library(unicorn-emulator SHARED ${SRC_FILES})
endif()

target_include_directories(unicorn-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(unicorn-emulator PUBLIC emulator)
target_link_libraries(unicorn-emulator PRIVATE unicorn emulator-common)
