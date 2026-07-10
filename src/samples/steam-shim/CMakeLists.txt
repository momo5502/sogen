# The guest steamclient shim: the game's own steam_api loads this in place of Valve's steamclient(64).dll.
# It forwards guest interface calls over the \\.\SogenSteam device to the host backend. Built version-exact:
# one guest-proxy object library per SDK snapshot (compiled against ONLY that snapshot's headers), linked
# into a single shim DLL alongside the version-agnostic entry points. Requires SOGEN_STEAM_ENABLED.
if(NOT SOGEN_STEAM_ENABLED)
  return()
endif()

# Version-agnostic base TUs: the flat Steamworks entry points + steamclient boundary (steam_shim.cpp, SDK-
# free so its exported SteamAPI_* don't clash with the SDK's inline ones) and the reverse-callback dispatch
# (steam_shim_reverse.cpp, built against the newest snapshot for the stable response interface types).
set(_base_srcs
  "${CMAKE_CURRENT_SOURCE_DIR}/steam_shim.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/steam_shim_reverse.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/steam_shim_runtime.hpp")

# Export the flat Steamworks entry points under undecorated names on both x86 and x64 (see steam_shim.def).
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  list(APPEND _base_srcs "${CMAKE_CURRENT_SOURCE_DIR}/steam_shim.def")
endif()

add_library(steam-shim SHARED ${_base_srcs})
sogen_assign_source_group(${_base_srcs})
target_link_libraries(steam-shim PRIVATE steam-bridge-protocol)
target_include_directories(steam-shim PRIVATE
  "${SOGEN_STEAM_BASE_DIR}" "${SOGEN_STEAM_GEN_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
sogen_target_disable_warnings(steam-shim)  # SDK headers + generated proxies are not warning-clean

# One isolated guest-proxy object library per version tag, each compiled against ONLY that snapshot's
# headers (identically-named ISteam* classes across snapshots can't share a translation unit) and linked in.
foreach(_entry ${SOGEN_STEAM_TAG_LIST})
  string(REPLACE "=" ";" _kv "${_entry}")
  list(GET _kv 0 _tag)
  list(GET _kv 1 _dir)
  set(STEAM_TAG "${_tag}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/steam_shim_tag.cpp.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/steam_shim_${_tag}.gen.cpp" @ONLY)
  add_library(steam-shim-${_tag} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/steam_shim_${_tag}.gen.cpp")
  sogen_target_disable_warnings(steam-shim-${_tag})
  target_include_directories(steam-shim-${_tag} PRIVATE
    "${_dir}" "${SOGEN_STEAM_GEN_DIR}/${_tag}" "${CMAKE_CURRENT_SOURCE_DIR}")
  target_sources(steam-shim PRIVATE $<TARGET_OBJECTS:steam-shim-${_tag}>)
endforeach()

# It replaces steamclient (loaded by the game's own steam_api): steamclient64.dll on x64, steamclient.dll
# on x86. The game's steam_api DLL is left untouched.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set_target_properties(steam-shim PROPERTIES OUTPUT_NAME "steamclient")
else()
  set_target_properties(steam-shim PROPERTIES OUTPUT_NAME "steamclient64")
endif()
