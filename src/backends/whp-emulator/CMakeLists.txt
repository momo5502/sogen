file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

if(SOGEN_BUILD_STATIC)
	add_library(whp-emulator STATIC ${SRC_FILES})
else()
    add_library(whp-emulator SHARED ${SRC_FILES})
endif()

target_include_directories(whp-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(whp-emulator PUBLIC emulator)
target_link_libraries(whp-emulator PRIVATE WinHvPlatform emulator-common)

momo_strip_target(whp-emulator)
