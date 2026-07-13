# The guest steamclient shim: the game's own steam_api loads this in place of Valve's steamclient(64).dll.
# It forwards guest interface calls over the \\.\SogenSteam device to the host backend. Built version-exact:
# one guest-proxy object library per SDK snapshot (compiled against ONLY that snapshot's headers), linked
# into a single shim DLL alongside the version-agnostic entry points. Requires SOGEN_STEAM_ENABLED.
if(NOT SOGEN_STEAM_ENABLED)
  return()
endif()

# Version-agnostic base TUs: the flat Steamworks entry points + steamclient boundary (steam_shim.cpp, SDK-
# free so its exported SteamAPI_* don't clash with the SDK's inline ones) and the reverse-callback dispatch
# (steam_shim_reverse.cpp, built against the newest snapshot for the stable response interface types). The
# per-version proxy TUs are generated into the binary dir and linked in as object libraries further down.
file(GLOB_RECURSE _base_srcs CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)
list(SORT _base_srcs)

# Export the flat Steamworks entry points under undecorated names on both x86 and x64 (see steam_shim.def).
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  list(APPEND _base_srcs "${CMAKE_CURRENT_SOURCE_DIR}/steam_shim.def")
endif()

# EXCLUDE_FROM_ALL: the standalone shim is an emulation-root artifact (built via the emulation-root-tools
# meta target), not part of the emulator build -- so the ordinary build jobs don't spend time on it (and
# don't hit the SDK's snprintf clash under MinGW gcc).
add_library(steam-shim SHARED EXCLUDE_FROM_ALL ${_base_srcs})
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
  add_library(steam-shim-${_tag} OBJECT EXCLUDE_FROM_ALL "${CMAKE_CURRENT_BINARY_DIR}/steam_shim_${_tag}.gen.cpp")
  target_link_libraries(steam-shim-${_tag} PRIVATE steam-bridge-protocol)  # shared response_type ids
  target_include_directories(steam-shim-${_tag} SYSTEM PRIVATE "${_dir}")  # SDK headers: not our warnings
  target_include_directories(steam-shim-${_tag} PRIVATE
    "${SOGEN_STEAM_GEN_DIR}/${_tag}" "${CMAKE_CURRENT_SOURCE_DIR}")
  sogen_target_generated_code_warnings(steam-shim-${_tag})
  target_sources(steam-shim PRIVATE $<TARGET_OBJECTS:steam-shim-${_tag}>)
endforeach()

# It replaces steamclient (loaded by the game's own steam_api): steamclient64.dll on x64, steamclient.dll
# on x86. The game's steam_api DLL is left untouched.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set_target_properties(steam-shim PROPERTIES OUTPUT_NAME "steamclient")
else()
  set_target_properties(steam-shim PROPERTIES OUTPUT_NAME "steamclient64")
endif()
