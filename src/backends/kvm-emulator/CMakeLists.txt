file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

if(SOGEN_BUILD_STATIC)
	add_library(kvm-emulator STATIC ${SRC_FILES})
else()
    add_library(kvm-emulator SHARED ${SRC_FILES})
endif()

target_include_directories(kvm-emulator INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

target_link_libraries(kvm-emulator PUBLIC emulator)
target_link_libraries(kvm-emulator PRIVATE emulator-common)

sogen_strip_target(kvm-emulator)
