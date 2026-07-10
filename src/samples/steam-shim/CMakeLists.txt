# The guest shim derives from the real SDK interfaces (via the generated proxies), so it needs the SDK.
if(NOT SOGEN_STEAMWORKS_SDK_DIR OR NOT EXISTS "${SOGEN_STEAMWORKS_SDK_DIR}/steam_api.h")
  return()
endif()

# Each old-SDK generation is compiled in its own TU against ITS headers only (identically-named ISteam*
# classes across SDK versions can't share a translation unit). Keep those TUs out of the main target.
set(_v105_tu "${CMAKE_CURRENT_SOURCE_DIR}/steam_shim_interfaces_v105.cpp")

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)
list(REMOVE_ITEM SRC_FILES "${_v105_tu}")
list(SORT SRC_FILES)

# Export the flat Steamworks entry points under undecorated names on both x86 and x64 (see steam_shim.def).
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  list(APPEND SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/steam_shim.def)
endif()

add_library(steam-shim SHARED ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})

target_link_libraries(steam-shim PRIVATE steam-bridge-protocol)
target_include_directories(steam-shim PRIVATE
  "${SOGEN_STEAMWORKS_SDK_DIR}"
  "${CMAKE_SOURCE_DIR}/src/steam-generated"
  "${CMAKE_CURRENT_SOURCE_DIR}")

# SDK headers + generated proxies are not warning-clean; opt out of the strict warnings-as-errors pass.
sogen_target_disable_warnings(steam-shim)

# Old-SDK tag: Steamworks 1.05 (SteamClient008 et al., what 2009-era games such as MW2 were built against).
# Its proxies are generated into src/steam-generated/v105 and compiled here against the 1.05 headers only,
# then linked in as objects. Absent the SDK/generation, the shim simply builds without legacy support.
if(SOGEN_STEAMWORKS_SDK_105_DIR AND EXISTS "${SOGEN_STEAMWORKS_SDK_105_DIR}/steam_api.h"
   AND EXISTS "${CMAKE_SOURCE_DIR}/src/steam-generated/v105/steam_shim_proxies.generated.hxx")
  add_library(steam-shim-v105 OBJECT "${_v105_tu}")
  target_include_directories(steam-shim-v105 PRIVATE
    "${SOGEN_STEAMWORKS_SDK_105_DIR}"
    "${CMAKE_SOURCE_DIR}/src/steam-generated/v105"
    "${CMAKE_CURRENT_SOURCE_DIR}")
  sogen_target_disable_warnings(steam-shim-v105)
  target_sources(steam-shim PRIVATE $<TARGET_OBJECTS:steam-shim-v105>)
  target_compile_definitions(steam-shim PRIVATE SOGEN_STEAM_HAVE_V105)
endif()

# It replaces steamclient (loaded by the game's own steam_api): steamclient64.dll on x64, steamclient.dll
# on x86. The game's steam_api DLL is left untouched.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set_target_properties(steam-shim PROPERTIES OUTPUT_NAME "steamclient")
else()
  set_target_properties(steam-shim PROPERTIES OUTPUT_NAME "steamclient64")
endif()
