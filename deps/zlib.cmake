set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(zlib)
target_compile_definitions(zlibstatic PUBLIC ZLIB_CONST=1)
target_include_directories(zlibstatic PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})

if (TARGET zlib)
    # Remove all properties and dependencies from the zlib shared lib target
    # (this will keep the zlibstatic target)
    set_target_properties(zlib PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set_target_properties(zlib PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD TRUE)
    message(STATUS "Target 'zlib' has been disabled.")
endif()
