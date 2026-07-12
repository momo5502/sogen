add_subdirectory(common)
add_subdirectory(gpu-bridge-protocol)
add_subdirectory(steam-bridge-protocol)

# --- Steam paravirtualization bridge -----------------------------------------------------------------
# Version-exact: EVERY Steamworks SDK snapshot Proton vendors is parsed with libclang (generate-time only)
# and compiled in isolation, so a game always gets the exact interface versions it was built against. The
# Valve-licensed headers (Steamworks SDK License -- not redistributable) are never committed: they are
# fetched on demand from a Proton checkout and code is generated from them. Nothing proprietary is stored
# in this tree or the build output beyond the generated marshalling glue. Disable with -DSOGEN_ENABLE_STEAM=OFF.
#
# proton_7.0 is the last Proton branch that still vendors the per-version steamworks_sdk_* snapshot dirs
# (8.0+ dropped them); it carries 78 snapshots spanning 099u..153a -- every interface a game may request.
option(SOGEN_ENABLE_STEAM "Build the Steam bridge (fetches Proton SDK snapshots; needs the libclang pip package)" ON)
set(SOGEN_STEAMWORKS_PROTON_REPO "https://github.com/ValveSoftware/Proton.git"
    CACHE STRING "Repo carrying vendored steamworks_sdk_* header snapshots (Valve headers come from this source)")
set(SOGEN_STEAMWORKS_PROTON_TAG "proton_7.0" CACHE STRING "Ref of SOGEN_STEAMWORKS_PROTON_REPO to fetch")

set(SOGEN_STEAM_ENABLED FALSE)
if(SOGEN_ENABLE_STEAM)
  find_package(Python3 COMPONENTS Interpreter)
  if(NOT Python3_Interpreter_FOUND)
    message(WARNING "SOGEN_ENABLE_STEAM: Python3 not found; Steam bridge disabled.")
  else()
    include(FetchContent)
    # Headers only: fetch shallow and skip Proton's heavy submodules (wine/dxvk/...). SOURCE_SUBDIR points at
    # a nonexistent path so MakeAvailable populates the checkout but never runs Proton's own build system --
    # we consume the raw vendored headers and nothing else.
    FetchContent_Declare(steamworks_snapshots GIT_REPOSITORY "${SOGEN_STEAMWORKS_PROTON_REPO}"
      GIT_TAG "${SOGEN_STEAMWORKS_PROTON_TAG}" GIT_SHALLOW TRUE GIT_SUBMODULES "" GIT_PROGRESS TRUE
      SOURCE_SUBDIR __sogen_headers_only__)
    FetchContent_MakeAvailable(steamworks_snapshots)
    set(_snap_root "${steamworks_snapshots_SOURCE_DIR}")
    if(EXISTS "${_snap_root}/lsteamclient")  # Proton keeps the snapshots under lsteamclient/
      set(_snap_root "${_snap_root}/lsteamclient")
    endif()

    # Build a version-exact tag per discovered snapshot; the newest one (sorts last) is the base the
    # hand-written, version-agnostic TUs compile against for stable callback/response interface types.
    file(GLOB _snap_dirs LIST_DIRECTORIES true "${_snap_root}/steamworks_sdk_*")
    list(SORT _snap_dirs)
    set(SOGEN_STEAM_TAG_LIST "")
    set(_gen_args "")
    set(SOGEN_STEAM_BASE_DIR "")
    foreach(_dir ${_snap_dirs})
      if(IS_DIRECTORY "${_dir}" AND EXISTS "${_dir}/steam_api.h")
        get_filename_component(_name "${_dir}" NAME)  # steamworks_sdk_105 -> tag v105
        string(REPLACE "steamworks_sdk_" "v" _tag "${_name}")
        list(APPEND SOGEN_STEAM_TAG_LIST "${_tag}=${_dir}")
        list(APPEND _gen_args "--tag" "${_tag}=${_dir}")
        set(SOGEN_STEAM_BASE_DIR "${_dir}")
      endif()
    endforeach()

    if(NOT SOGEN_STEAM_TAG_LIST)
      message(WARNING "Steam: no steamworks_sdk_* snapshots under '${_snap_root}'; Steam bridge disabled.")
    else()
      math(EXPR _bits "${CMAKE_SIZEOF_VOID_P} * 8")  # parse per target width (32-bit guest vs 64-bit host)
      set(SOGEN_STEAM_GEN_DIR "${CMAKE_BINARY_DIR}/steam-generated")
      list(LENGTH SOGEN_STEAM_TAG_LIST _tag_count)
      message(STATUS "Steam bridge: generating ${_bits}-bit marshalling for ${_tag_count} SDK snapshot(s)...")
      execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/steam-bridge-generator/generate.py"
                ${_gen_args} --out-dir "${SOGEN_STEAM_GEN_DIR}" --bits "${_bits}"
        RESULT_VARIABLE _gen_res ERROR_VARIABLE _gen_err)
      if(NOT _gen_res EQUAL 0)
        message(WARNING "Steam bridge generator failed (is the 'libclang' pip package installed?):\n${_gen_err}\n"
                        "Steam bridge disabled.")
      else()
        message(STATUS "Steam bridge enabled (base ${SOGEN_STEAM_BASE_DIR}).")
        set(SOGEN_STEAM_ENABLED TRUE)
      endif()
    endif()
  endif()
endif()

add_subdirectory(steam-host-backend)
add_subdirectory(vulkan-bridge-marshal)
add_subdirectory(emulator-platform)
add_subdirectory(disassembler)
add_subdirectory(emulator)
add_subdirectory(gdb-stub)
add_subdirectory(windows-emulator)
add_subdirectory(windows-gdb-stub)
add_subdirectory(linux-emulator)
add_subdirectory(linux-gdb-stub)
add_subdirectory(backend-selection)

if(SOGEN_ENABLE_PYTHON_BINDINGS)
  add_subdirectory(python-bindings)
endif()

sogen_add_subdirectory_and_get_targets("backends" BACKEND_TARGETS)
sogen_targets_set_folder("backends" ${BACKEND_TARGETS})

set(_SOGEN_BUILD_STANDALONE_TARGETS ON)
if(SOGEN_BUILD_STATIC)
  # if static build, tools are opt in
  set(_SOGEN_BUILD_STANDALONE_TARGETS ${SOGEN_BUILD_TOOLS})
endif()

if(_SOGEN_BUILD_STANDALONE_TARGETS)
  add_subdirectory(windows-analyzer)
  add_subdirectory(linux-analyzer)
  add_subdirectory(analyzer)
  add_subdirectory(debugger)
  add_subdirectory(linux-debugger)
  add_subdirectory(fuzzing-engine)
  add_subdirectory(fuzzer)
  if(SOGEN_ENABLE_FUZZING)
    add_subdirectory(windows-emulator-fuzz)
  endif()
  add_subdirectory(windows-emulator-test)
  add_subdirectory(linux-emulator-test)

  if((WIN32 OR LINUX) AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    sogen_add_subdirectory_and_get_targets("tools" TOOL_TARGETS)
    sogen_targets_set_folder("tools" ${TOOL_TARGETS})
  endif()

  if(WIN32)
    sogen_add_subdirectory_and_get_targets("samples" SAMPLE_TARGETS)
    sogen_targets_set_folder("samples" ${SAMPLE_TARGETS})
  endif()
endif()
