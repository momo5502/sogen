include_guard()

##########################################

function(momo_silence_deprecation_warnings)
    set(CMAKE_WARN_DEPRECATED_OLD ${CMAKE_WARN_DEPRECATED} PARENT_SCOPE)
    set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
endfunction()

##########################################

function(momo_restore_deprecation_warnings)
    set(CMAKE_WARN_DEPRECATED ${CMAKE_WARN_DEPRECATED_OLD} CACHE BOOL "" FORCE)
endfunction()

##########################################

function(momo_target_exclude_from_all target)
  set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL 1)
  #set_target_properties(${target} PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD 1)
endfunction()

##########################################

function(momo_targets_exclude_from_all)
  foreach(target ${ARGV})
    momo_target_exclude_from_all(${target})
  endforeach()
endfunction()

##########################################

function(momo_target_set_folder folder target)
  #get_target_property(CURRENT_FOLDER ${target} FOLDER)
  #if(NOT CURRENT_FOLDER)
    set_target_properties(${target} PROPERTIES FOLDER "${folder}")
  #endif()
endfunction()

##########################################

function(momo_targets_set_folder folder)
  foreach(target ${ARGN})
    momo_target_set_folder(${folder} ${target})
  endforeach()
endfunction()

##########################################

function(momo_target_disable_compile_commands target)
  set_target_properties(${target} PROPERTIES EXPORT_COMPILE_COMMANDS OFF)
endfunction()

##########################################

function(momo_targets_disable_compile_commands)
  foreach(target ${ARGV})
    momo_target_disable_compile_commands(${target})
  endforeach()
endfunction()

##########################################

function(momo_target_expose_includes target)
  get_target_property(target_type ${target} TYPE)
  if("${target_type}" STREQUAL "UTILITY")
    return()
  endif()

  get_target_property(TARGET_SOURCE_DIR ${target} SOURCE_DIR)
  target_include_directories(${target} INTERFACE ${TARGET_SOURCE_DIR}/..)
endfunction()

##########################################

function(momo_targets_expose_includes)
  foreach(target ${ARGV})
  momo_target_expose_includes(${target})
  endforeach()
endfunction()

##########################################

function(momo_target_compile_options language target mode)
  foreach(compile_option ${ARGN})
    target_compile_options(${target} ${mode}
      $<$<COMPILE_LANGUAGE:${language}>:${compile_option}>
    )
  endforeach()
endfunction()

##########################################

function(momo_target_c_and_cxx_compile_options)
  momo_target_compile_options(C ${ARGV})
  momo_target_compile_options(CXX ${ARGV})
endfunction()

##########################################

macro(momo_target_remove_compile_option target option)
    get_target_property(target_flags ${target} COMPILE_OPTIONS)
    if(target_flags)
        list(REMOVE_ITEM target_flags ${option})
        set_target_properties(${target} PROPERTIES COMPILE_OPTIONS "${target_flags}")
    endif()

    get_target_property(target_interface_flags ${target} INTERFACE_COMPILE_OPTIONS)
    if(target_interface_flags)
        list(REMOVE_ITEM target_interface_flags ${option})
        set_target_properties(${target} PROPERTIES INTERFACE_COMPILE_OPTIONS "${target_interface_flags}")
    endif()
endmacro()

##########################################

macro(momo_target_remove_compile_options target)
  foreach(option ${ARGV})
    momo_target_remove_compile_option(${target} ${option})
  endforeach()
endmacro()

##########################################

function(momo_add_compile_options language)
  foreach(option ${ARGN})
    add_compile_options(
      $<$<COMPILE_LANGUAGE:${language}>:${option}>
    )
  endforeach()
endfunction()

##########################################

function(momo_add_release_compile_options language)
  foreach(option ${ARGN})
    add_compile_options(
      $<$<COMPILE_LANGUAGE:${language}>:$<$<CONFIG:RELEASE>:${option}>>
      $<$<COMPILE_LANGUAGE:${language}>:$<$<CONFIG:RELWITHDEBINFO>:${option}>>
    )
  endforeach()
endfunction()

##########################################

function(momo_add_release_link_options)
  foreach(option ${ARGN})
    add_link_options(
      $<$<CONFIG:RELEASE>:${option}>
      $<$<CONFIG:RELWITHDEBINFO>:${option}>
    )
  endforeach()
endfunction()

##########################################

function(momo_add_c_and_cxx_compile_options)
  momo_add_compile_options(C ${ARGV})
  momo_add_compile_options(CXX ${ARGV})
endfunction()

##########################################

function(momo_add_c_and_cxx_release_compile_options)
  momo_add_release_compile_options(C ${ARGV})
  momo_add_release_compile_options(CXX ${ARGV})
endfunction()

##########################################

function(momo_target_disable_warnings target)
  get_target_property(target_type ${target} TYPE)
  if(("${target_type}" STREQUAL "INTERFACE_LIBRARY") OR ("${target_type}" STREQUAL "UTILITY"))
    return()
  endif()

  momo_target_remove_compile_options(${target} /W3 -W3 /W4 -W4)

  if(MSVC)
  set(compile_options
    /W0
    /D_CRT_SECURE_NO_WARNINGS=1
  )
  endif()

  momo_target_c_and_cxx_compile_options(${target} PRIVATE ${compile_options})

  set_target_properties(${target} PROPERTIES MOMO_WARNINGS_DISABLE ON)
endfunction()

##########################################

function(momo_targets_disable_warnings)
  foreach(target ${ARGV})
    momo_target_disable_warnings(${target})
  endforeach()
endfunction()

##########################################

function(momo_target_set_warnings_as_errors target)
  get_target_property(target_type ${target} TYPE)
  if(("${target_type}" STREQUAL "INTERFACE_LIBRARY") OR ("${target_type}" STREQUAL "UTILITY"))
    return()
  endif()

  get_target_property(warnings_disabled ${target} MOMO_WARNINGS_DISABLE)
  if(warnings_disabled)
    return()
  endif()

  set(compile_options -Wall -Wextra
   #-Wconversion
   -pedantic -Werror -Wno-comment)

  if(MSVC)
    set(compile_options /W4 /WX)
  endif()

  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:$<$<CONFIG:RELEASE>:${compile_options}>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CONFIG:RELEASE>:${compile_options}>>
    $<$<COMPILE_LANGUAGE:C>:$<$<CONFIG:RELWITHDEBINFO>:${compile_options}>>
    $<$<COMPILE_LANGUAGE:CXX>:$<$<CONFIG:RELWITHDEBINFO>:${compile_options}>>
  )
endfunction()

##########################################

function(momo_targets_set_warnings_as_errors)
  foreach(target ${ARGV})
    momo_target_set_warnings_as_errors(${target})
  endforeach()
endfunction()

##########################################

function(momo_get_all_targets var)
    set(targets)
    momo_get_all_targets_recursive(targets ${CMAKE_CURRENT_SOURCE_DIR})
    set(${var} ${targets} PARENT_SCOPE)
endfunction()

##########################################

macro(momo_get_all_targets_recursive targets dir)
    get_property(subdirectories DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(subdir ${subdirectories})
        momo_get_all_targets_recursive(${targets} ${subdir})
    endforeach()

    get_property(current_targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    list(APPEND ${targets} ${current_targets})
endmacro()

##########################################

macro(momo_list_difference list_a list_to_remove result)
  set(${result} ${list_a})
  list(REMOVE_ITEM ${result} ${list_to_remove})
endmacro()

##########################################

macro(momo_set_artifact_directory directory)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${directory})
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${directory})
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${directory})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${directory})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${directory})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${directory})
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${directory})
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG ${directory})
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${directory})
endmacro()

##########################################

macro(momo_set_new_artifact_directory)
  get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(IS_MULTI_CONFIG)
      set(ARTIFACT_FOLDER_NAME "artifacts-$<LOWER_CASE:$<CONFIG>>")
  else()
      set(ARTIFACT_FOLDER_NAME "artifacts")
  endif()

  set(ARTIFACT_DIRECTORY "${CMAKE_BINARY_DIR}/${ARTIFACT_FOLDER_NAME}")
  momo_set_artifact_directory(${ARTIFACT_DIRECTORY})
endmacro()

##########################################

macro(momo_add_subdirectory_and_get_targets directory targets)
  momo_get_all_targets(EXISTING_TARGETS)
  add_subdirectory(${directory})
  momo_get_all_targets(ALL_TARGETS)

  momo_list_difference("${ALL_TARGETS}" "${EXISTING_TARGETS}" ${targets})
endmacro()

##########################################

macro(momo_target_include_libraries target mode)
  foreach(inc_target ${ARGN})
    target_include_directories(${target} ${mode}
      $<TARGET_PROPERTY:${inc_target},INTERFACE_INCLUDE_DIRECTORIES>
      $<TARGET_PROPERTY:${inc_target},PUBLIC_INCLUDE_DIRECTORIES>
    )
  endforeach()
endmacro()

##########################################

function(momo_strip_target target)
  if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    return()
  endif()

  if(NOT MSVC)
    # TODO: detect LLVM IR bitcode and abort
    return()
    if(NOT DEFINED STRIP_COMMAND)
      set(STRIP_COMMAND strip)
    endif()

    if(NOT DEFINED STRIP_FLAGS)
      set(STRIP_FLAGS -g -s)
      if(OSX)
        set(STRIP_FLAGS -x)
      endif()
    endif()

    set(IN_FILE "$<TARGET_FILE:${target}>")
    set(OUT_FILE "$<TARGET_FILE_DIR:${target}>/$<TARGET_FILE_PREFIX:${target}>$<TARGET_FILE_BASE_NAME:${target}>-unstripped$<TARGET_FILE_SUFFIX:${target}>")

    add_custom_command(TARGET ${target} POST_BUILD 
      COMMAND ${CMAKE_COMMAND} -E copy ${IN_FILE} ${OUT_FILE}
      COMMAND "${STRIP_COMMAND}" ${STRIP_FLAGS} "${IN_FILE}"
      COMMENT "Strippping ${target}"
    )
  endif()
endfunction()

##########################################

macro(momo_assign_source_group)
  source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${ARGN})
endmacro()
