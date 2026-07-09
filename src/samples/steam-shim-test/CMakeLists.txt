# Uses the real SDK interface types, so it needs the SDK (same gate as the shim it drives).
if(NOT SOGEN_STEAMWORKS_SDK_DIR OR NOT EXISTS "${SOGEN_STEAMWORKS_SDK_DIR}/steam_api.h")
  return()
endif()

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)
list(SORT SRC_FILES)

add_executable(steam-shim-test ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_include_directories(steam-shim-test PRIVATE "${SOGEN_STEAMWORKS_SDK_DIR}")
sogen_target_disable_warnings(steam-shim-test)
