set(UNICORN_ARCH "x86" CACHE STRING "")
add_subdirectory(unicorn)

##########################################

option(BASE64_ENABLE_TESTING "" OFF)
add_subdirectory(base64)

##########################################

option(FLATBUFFERS_BUILD_TESTS "" OFF)
option(FLATBUFFERS_INSTALL "" OFF)
add_subdirectory(flatbuffers)

if(MSVC)
target_compile_options(flatc PRIVATE /MD$<$<CONFIG:Debug>:d>)
endif()

##########################################

if(SOGEN_ENABLE_REFLECTION)
  add_library(reflect INTERFACE)
  target_include_directories(reflect INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/reflect"
  )
endif()

##########################################

add_subdirectory(minidump_cpp)

##########################################

if(SOGEN_ENABLE_SDL3 AND NOT EMSCRIPTEN AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/SDL/CMakeLists.txt")
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(SDL)
endif()

##########################################

if(SOGEN_ENABLE_PYTHON_BINDINGS)
  if (NOT TARGET Python::Module)
    if(CMAKE_VERSION VERSION_LESS 3.18)
      set(_SOGEN_PYTHON_DEV_MODULE Development)
    else()
      set(_SOGEN_PYTHON_DEV_MODULE Development.Module)
    endif()

    find_package(Python 3.9 REQUIRED COMPONENTS Interpreter ${_SOGEN_PYTHON_DEV_MODULE} OPTIONAL_COMPONENTS Development.SABIModule)
  endif()

  if (NOT TARGET Python::Module)
    add_library(Python::Module INTERFACE IMPORTED)
    if (TARGET Python::SABIModule)
      set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::SABIModule)
    elseif (TARGET Python::Python)
      set_property(TARGET Python::Module PROPERTY INTERFACE_LINK_LIBRARIES Python::Python)
    endif()
  endif()

  add_subdirectory(nanobind)
endif()

##########################################

option(CAPSTONE_BUILD_MACOS_THIN "" ON)
option(CAPSTONE_X86_SUPPORT "" ON)
option(CAPSTONE_X86_ATT_DISABLE "" ON)
option(CAPSTONE_ARCHITECTURE_DEFAULT "" OFF)
option(CAPSTONE_BUILD_STATIC_MSVC_RUNTIME "" OFF)
add_subdirectory(capstone)

##########################################

include(googletest.cmake)
include(zstd.cmake)
