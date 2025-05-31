include_guard()

if(CMAKE_GENERATOR STREQUAL "Ninja")
    find_program(SCCACHE sccache)

    if (SCCACHE)
        file(TO_CMAKE_PATH "${SCCACHE}" SCCACHE)
        set(CMAKE_C_COMPILER_LAUNCHER ${SCCACHE})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE})
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)

        if(POLICY CMP0141)
            cmake_policy(SET CMP0141 NEW)
        endif()
    endif()
endif()