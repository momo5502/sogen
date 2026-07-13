# The Steam host backend: loads the host Steam client (steamclient64.dll / .so / .dylib) and runs the
# generated marshalling thunks. It runs inside the 64-bit emulator host, so it is only meaningful for a
# 64-bit build; the 32-bit build only produces the guest shim. Built when SOGEN_STEAM_ENABLED (set in
# ../cmake/steam-bridge.cmake after code generation).
if(NOT SOGEN_STEAM_ENABLED OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  return()
endif()

# Base TU: the C ABI + host client bring-up + reverse-callback proxies (stable response interfaces), built
# against the newest snapshot. It does not include any version's thunks -- those live in the per-tag TUs.
add_library(steam-host-backend STATIC steam_host_backend.cpp steam_reverse.cpp)
sogen_target_disable_warnings(steam-host-backend)  # SDK headers + generated code are not warning-clean
target_link_libraries(steam-host-backend PRIVATE ${CMAKE_DL_LIBS})  # dlopen/dlsym on Linux/macOS
target_link_libraries(steam-host-backend PRIVATE steam-bridge-protocol)  # shared response_type ids
target_include_directories(steam-host-backend PRIVATE
  "${SOGEN_STEAM_BASE_DIR}" "${SOGEN_STEAM_GEN_DIR}" "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(steam-host-backend PUBLIC "${CMAKE_CURRENT_LIST_DIR}")  # the C ABI header

# One isolated dispatch object library per version tag, each compiled against ONLY that snapshot's headers
# (identically-named ISteam* classes across snapshots can't share a translation unit) and linked in.
foreach(_entry ${SOGEN_STEAM_TAG_LIST})
  string(REPLACE "=" ";" _kv "${_entry}")
  list(GET _kv 0 _tag)
  list(GET _kv 1 _dir)
  set(STEAM_TAG "${_tag}")
  configure_file("${CMAKE_CURRENT_LIST_DIR}/steam_host_tag.cpp.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/steam_host_${_tag}.gen.cpp" @ONLY)
  add_library(steam-host-${_tag} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/steam_host_${_tag}.gen.cpp")
  sogen_target_disable_warnings(steam-host-${_tag})
  target_link_libraries(steam-host-${_tag} PRIVATE steam-bridge-protocol)  # shared response_type ids
  target_include_directories(steam-host-${_tag} PRIVATE
    "${_dir}" "${SOGEN_STEAM_GEN_DIR}/${_tag}" "${CMAKE_CURRENT_LIST_DIR}")
  target_sources(steam-host-backend PRIVATE $<TARGET_OBJECTS:steam-host-${_tag}>)
endforeach()
