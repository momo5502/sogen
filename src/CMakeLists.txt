add_subdirectory(common)
add_subdirectory(gpu-bridge-protocol)
add_subdirectory(steam-bridge-protocol)

# Path to the Steamworks SDK's public/steam directory. When set (and the generated code is present),
# the real Steam paravirtualization backend is built; otherwise the device falls back to a null backend.
set(SOGEN_STEAMWORKS_SDK_DIR "" CACHE PATH "Steamworks SDK public/steam dir (enables the real Steam bridge)")
# Optional old-SDK header dir (e.g. lsteamclient/steamworks_sdk_105) enabling legacy interface versions
# (SteamClient008 etc.) that pre-2010 games request. Generate its proxies with generate.py --old-sdk v105=...
set(SOGEN_STEAMWORKS_SDK_105_DIR "" CACHE PATH "Steamworks SDK 1.05 header dir (enables legacy Steam interfaces)")
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
