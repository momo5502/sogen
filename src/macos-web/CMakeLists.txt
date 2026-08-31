if(NOT EMSCRIPTEN)
  return()
endif()

# One guest architecture per wasm module, and it must be aarch64.
#
# With x86 also enabled, both softmmu targets end up in one module and an aarch64 translation block
# resolves the *x86_64* store helper. That helper reads an x86 CPUArchState and TLB, produces a host
# address outside linear memory, and the module traps with "memory access out of bounds" four
# instructions into dyld. Nothing reports a problem before that: it links cleanly and static binaries
# run, because their stores hit the inlined fast path and never call the helper.
#
# Reproduced both ways: x86;aarch64 traps, aarch64 alone runs /bin/echo through real dyld to exit 0.
if(NOT UNICORN_ARCH STREQUAL "aarch64")
  message(WARNING
    "macos-web needs -DUNICORN_ARCH=aarch64 (currently '${UNICORN_ARCH}'). "
    "A multi-architecture wasm module mis-resolves Unicorn's memory helpers and faults inside dyld. "
    "Skipping the target.")
  return()
endif()

add_executable(macos-web main.cpp)

target_link_libraries(macos-web PRIVATE macos-emulator backend-selection emulator-common ui-backends)

target_link_options(macos-web PRIVATE
  # Function names but no DWARF. The build type is RelWithDebInfo, whose -g asks for DWARF, and DWARF is
  # what a browser downloads: it takes this module from 4.9 MB to 45 MB. Measured, and the reason this is
  # only about size: -g0, -g2 and an added -O3 all run the paint demo in 5.7-5.8 s, so the
  # "limited binaryen optimizations because DWARF info requested" the link warns about costs nothing at
  # run time. Names are kept because a wasm stack trace and a CPU profile are unreadable without them.
  "-g2"
  "-sALLOW_MEMORY_GROWTH=1"
  "-sEXPORTED_RUNTIME_METHODS=['callMain','FS','WORKERFS']"

  # The frame pump yields with emscripten_sleep(0) from the syscall hook, so an unwind has to record
  # every instrumented frame between main() and the guest's svc -- the emulator, the syscall dispatcher
  # and the whole TCI interpreter below them. Emscripten's 4 KiB default holds a few hundred frames and
  # is sized for a shallow main loop; overflowing it aborts the module.
  "-sASYNCIFY_STACK_SIZE=131072"

  # A macOS root is gigabytes and the dyld shared cache alone is 5.4 GB, so the guest filesystem cannot
  # be a copy of it. WORKERFS mounts the File objects the user picked and reads slices of them through
  # FileReaderSync on demand, which is what lets the emulator open and read the cache without any of it
  # being held in linear memory.
  "-lworkerfs.js"
  "-sMODULARIZE=1"
  "-sEXPORT_ES6=1"
  "-sEXPORT_NAME=createSogenMacos"
  "-sENVIRONMENT=web,worker,node"
  "-sINVOKE_RUN=0"
  "--embed-file=${CMAKE_CURRENT_LIST_DIR}/../macos-emulator-test/fixtures/macho_trace_arm64@/demo/macho_trace_arm64"

  # The GUI demo. Dynamically linked against SkyLight, so it only runs once the page has attached a macOS
  # root; the static trace demo above needs nothing.
  "--embed-file=${CMAKE_CURRENT_LIST_DIR}/../macos-emulator-test/fixtures/macho_calcdemo_arm64@/demo/macho_calcdemo_arm64"
  "--embed-file=${CMAKE_CURRENT_LIST_DIR}/../macos-emulator-test/fixtures/macho_paintprobe_arm64@/demo/macho_paintprobe_arm64"
)

configure_file(index.html "${CMAKE_BINARY_DIR}/artifacts/index.html" COPYONLY)
configure_file(app.js "${CMAKE_BINARY_DIR}/artifacts/app.js" COPYONLY)
configure_file(emulator-worker.js "${CMAKE_BINARY_DIR}/artifacts/emulator-worker.js" COPYONLY)
configure_file(sw.js "${CMAKE_BINARY_DIR}/artifacts/sw.js" COPYONLY)
configure_file(app.css "${CMAKE_BINARY_DIR}/artifacts/app.css" COPYONLY)
configure_file(capability-probe.html "${CMAKE_BINARY_DIR}/artifacts/capability-probe.html" COPYONLY)
configure_file(capability-probe-worker.js "${CMAKE_BINARY_DIR}/artifacts/capability-probe-worker.js" COPYONLY)

# page/ serves public/ at the origin root, which is where the worker and the root-cache service worker
# have to be for their scope to cover the emulator's own Web Worker. A worker the service worker does not
# control bypasses the cache silently, which reads as caching that simply does not work.
set(MACOS_WEB_PAGE_PUBLIC "${CMAKE_SOURCE_DIR}/page/public")

add_custom_command(TARGET macos-web POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${MACOS_WEB_PAGE_PUBLIC}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE_DIR:macos-web>/macos-web.js" "${MACOS_WEB_PAGE_PUBLIC}/macos-analyzer.js"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE_DIR:macos-web>/macos-web.wasm" "${MACOS_WEB_PAGE_PUBLIC}/macos-analyzer.wasm"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_LIST_DIR}/capability-probe.html" "${MACOS_WEB_PAGE_PUBLIC}/capability-probe.html"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_LIST_DIR}/capability-probe-worker.js" "${MACOS_WEB_PAGE_PUBLIC}/capability-probe-worker.js"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_LIST_DIR}/app.css" "${MACOS_WEB_PAGE_PUBLIC}/app.css"
  COMMENT "Publishing the macOS wasm module and capability probe into page/public")
