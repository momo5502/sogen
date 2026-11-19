# Steam paravirtualization bridge codegen. Parses Proton's per-version Steamworks SDK snapshots with
# libclang at configure time and emits version-exact marshalling into ${SOGEN_STEAM_GEN_DIR}, setting
# SOGEN_STEAM_ENABLED for the backend/shim targets. The host backend loads the real Steam client
# dynamically (steamclient64.dll / steamclient.so / steamclient.dylib), so it builds on the desktop hosts
# a real Steam runs on (Windows, Linux, macOS) -- not the mobile/web targets. The Valve-licensed headers
# are fetched, never committed.

option(SOGEN_ENABLE_STEAM "Build the Steam bridge (fetches Proton SDK snapshots; needs the libclang pip package)" ON)
# Proton's proton_11.0 branch as of 1f089f56. Its steamworks_sdk_* directories carry the header snapshots.
set(SOGEN_STEAMWORKS_PROTON_URL
    "https://github.com/ValveSoftware/Proton/archive/1f089f5610a882c8bfcb4b58a3c86ee88bb4b9f1.tar.gz"
    CACHE STRING "Source archive carrying vendored steamworks_sdk_* header snapshots")
set(SOGEN_STEAMWORKS_PROTON_SHA256 "46aa87e3c190a3fb0c0155924ce23e6c2bade0c83dd0109143b8c9f1be10a193"
    CACHE STRING "SHA256 of SOGEN_STEAMWORKS_PROTON_URL")

# Answers the guest's Steam calls from a provider inside the emulator instead of a host Steam client, for
# hosts without one (Android).
option(SOGEN_ENABLE_STEAM_LOCAL_PROVIDER "Answer guest Steam calls from a built-in provider instead of a Steam client" OFF)
if(SOGEN_ENABLE_STEAM_LOCAL_PROVIDER AND NOT SOGEN_ENABLE_STEAM)
  message(FATAL_ERROR "SOGEN_ENABLE_STEAM_LOCAL_PROVIDER needs SOGEN_ENABLE_STEAM.")
endif()

# Desktop hosts where a real Steam client exists (macOS = Darwin, excluding iOS).
set(SOGEN_STEAM_HOST_PLATFORM FALSE)
if(WIN32 OR LINUX OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR SOGEN_ENABLE_STEAM_LOCAL_PROVIDER)
  set(SOGEN_STEAM_HOST_PLATFORM TRUE)
endif()

# A build that asked for the provider must not quietly come out without Steam.
function(sogen_steam_unavailable reason)
  if(SOGEN_ENABLE_STEAM_LOCAL_PROVIDER)
    message(FATAL_ERROR "${reason}")
  else()
    message(WARNING "${reason}")
  endif()
endfunction()

set(SOGEN_STEAM_ENABLED FALSE)
if(SOGEN_ENABLE_STEAM AND SOGEN_STEAM_HOST_PLATFORM)
  find_package(Python3 COMPONENTS Interpreter)
  if(NOT Python3_Interpreter_FOUND)
    sogen_steam_unavailable("SOGEN_ENABLE_STEAM: Python3 not found; Steam bridge disabled.")
  else()
    include(FetchContent)
    # Headers only: SOURCE_SUBDIR points at a nonexistent path so the archive is extracted but Proton's own
    # build never runs.
    FetchContent_Declare(steamworks_snapshots URL "${SOGEN_STEAMWORKS_PROTON_URL}"
      URL_HASH "SHA256=${SOGEN_STEAMWORKS_PROTON_SHA256}" SOURCE_SUBDIR __sogen_headers_only__)
    FetchContent_MakeAvailable(steamworks_snapshots)
    set(_snap_root "${steamworks_snapshots_SOURCE_DIR}")
    if(EXISTS "${_snap_root}/lsteamclient")
      set(_snap_root "${_snap_root}/lsteamclient")
    endif()

    # One version-exact tag per snapshot; the newest (sorts last) is the base the version-agnostic TUs
    # compile against for stable callback/response types.
    file(GLOB _snap_dirs LIST_DIRECTORIES true "${_snap_root}/steamworks_sdk_*")
    list(SORT _snap_dirs)
    set(SOGEN_STEAM_TAG_LIST "")
    set(_gen_args "")
    set(SOGEN_STEAM_BASE_DIR "")
    foreach(_dir ${_snap_dirs})
      if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/steam_api.h")
        get_filename_component(_name "${_dir}" NAME)
        string(REPLACE "steamworks_sdk_" "v" _tag "${_name}")
        list(APPEND SOGEN_STEAM_TAG_LIST "${_tag}=${_dir}")
        list(APPEND _gen_args "--tag" "${_tag}=${_dir}")
        set(SOGEN_STEAM_BASE_DIR "${_dir}")
      endif()
    endforeach()

    if(NOT SOGEN_STEAM_TAG_LIST)
      sogen_steam_unavailable("Steam: no steamworks_sdk_* snapshots under '${_snap_root}'; Steam bridge disabled.")
    else()
      math(EXPR _bits "${CMAKE_SIZEOF_VOID_P} * 8")
      set(SOGEN_STEAM_GEN_DIR "${CMAKE_BINARY_DIR}/steam-generated")
      set(_gen_py "${CMAKE_CURRENT_SOURCE_DIR}/tools/steam-bridge-generator/generate.py")
      list(LENGTH SOGEN_STEAM_TAG_LIST _tag_count)

      # The generated output is fully determined by the generator script, the target width, and the SDK
      # snapshot set (which only changes when the Proton headers are fetched from a new archive). Fingerprint
      # those into a stamp and skip the run -- libclang parses every snapshot, so it is by far the slowest
      # part of configure -- whenever the stamp is unchanged since the last successful generation.
      file(SHA256 "${_gen_py}" _gen_py_hash)
      set(_gen_stamp "gen=${_gen_py_hash};bits=${_bits};proton=${SOGEN_STEAMWORKS_PROTON_SHA256}")
      string(APPEND _gen_stamp ";tags=${SOGEN_STEAM_TAG_LIST}")
      set(_gen_stamp_file "${SOGEN_STEAM_GEN_DIR}/.inputs.stamp")
      set(_do_generate TRUE)
      if(EXISTS "${_gen_stamp_file}" AND EXISTS "${SOGEN_STEAM_GEN_DIR}/steam_tags.generated.hxx")
        file(READ "${_gen_stamp_file}" _prev_stamp)
        if(_prev_stamp STREQUAL _gen_stamp)
          set(_do_generate FALSE)
        endif()
      endif()

      if(NOT _do_generate)
        message(STATUS "Steam bridge: generated marshalling up to date for ${_tag_count} snapshot(s); skipping codegen.")
        set(SOGEN_STEAM_ENABLED TRUE)
      else()
        message(STATUS "Steam bridge: generating ${_bits}-bit marshalling for ${_tag_count} SDK snapshot(s)...")
        execute_process(
          COMMAND "${Python3_EXECUTABLE}" "${_gen_py}"
                  ${_gen_args} --out-dir "${SOGEN_STEAM_GEN_DIR}" --bits "${_bits}"
          RESULT_VARIABLE _gen_res ERROR_VARIABLE _gen_err)
        if(NOT _gen_res EQUAL 0)
          sogen_steam_unavailable("Steam bridge generator failed (is the 'libclang' pip package installed?):\n${_gen_err}\n"
                                  "Steam bridge disabled.")
        else()
          file(WRITE "${_gen_stamp_file}" "${_gen_stamp}")
          message(STATUS "Steam bridge enabled (base ${SOGEN_STEAM_BASE_DIR}).")
          set(SOGEN_STEAM_ENABLED TRUE)
        endif()
      endif()
    endif()
  endif()
endif()
