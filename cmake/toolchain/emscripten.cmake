set(CMAKE_SYSTEM_NAME Emscripten)
set(CMAKE_SYSTEM_VERSION 1)

# Specify the cross-compilers
set(CMAKE_C_COMPILER emcc)
set(CMAKE_CXX_COMPILER em++)

# Set the Emscripten root directory
set(EMSCRIPTEN_ROOT_PATH $ENV{EMSDK}/upstream/emscripten)

# Set the Emscripten toolchain file
set(CMAKE_SYSROOT ${EMSCRIPTEN_ROOT_PATH}/system)

# Set the Emscripten include directories
set(CMAKE_FIND_ROOT_PATH ${EMSCRIPTEN_ROOT_PATH}/system/include)

# Set the Emscripten library directories
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Set the Emscripten linker
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s USE_SDL=2 -s USE_SDL_MIXER=2")

# Set the Emscripten runtime
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --shell-file shell_minimal.html")

# Set the Emscripten optimization flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")

# Set the Emscripten debug flags
set(CMAKE_BUILD_TYPE Release)

# Set the Emscripten output format
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -o <TARGET>.html")

# Set the Emscripten file extensions
set(CMAKE_EXECUTABLE_SUFFIX ".js")

# Set the Emscripten runtime options
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s EXPORTED_FUNCTIONS='[_main]' -s EXPORTED_RUNTIME_METHODS='[\"cwrap\"]'")