# The real Steam bridge backend links against the Steamworks SDK headers (Valve-licensed, not shipped)
# and generated marshalling code. It is built only when the SDK path is provided.
if(NOT SOGEN_STEAMWORKS_SDK_DIR)
  message(STATUS "SOGEN_STEAMWORKS_SDK_DIR not set: building without the real Steam bridge (null backend).")
  return()
endif()

if(NOT EXISTS "${SOGEN_STEAMWORKS_SDK_DIR}/steam_api.h")
  message(WARNING "SOGEN_STEAMWORKS_SDK_DIR='${SOGEN_STEAMWORKS_SDK_DIR}' has no steam_api.h; skipping Steam bridge.")
  return()
endif()

set(_steam_gen "${CMAKE_SOURCE_DIR}/src/steam-generated")
if(NOT EXISTS "${_steam_gen}/steam_host_thunks.generated.hxx")
  message(FATAL_ERROR
    "Generated Steam bridge code is missing. Generate it (kept out of the tree; it is SDK-derived):\n"
    "  python src/tools/steam-bridge-generator/generate.py --sdk \"${SOGEN_STEAMWORKS_SDK_DIR}\" --out-dir \"${_steam_gen}\"")
endif()

add_library(steam-host-backend STATIC steam_host_backend.cpp steam_reverse.cpp)

# SDK headers + generated code are not warning-clean; opt out of the strict warnings-as-errors pass.
sogen_target_disable_warnings(steam-host-backend)

target_include_directories(steam-host-backend PRIVATE "${SOGEN_STEAMWORKS_SDK_DIR}" "${_steam_gen}")
target_include_directories(steam-host-backend PUBLIC "${CMAKE_CURRENT_LIST_DIR}")  # the C ABI header
