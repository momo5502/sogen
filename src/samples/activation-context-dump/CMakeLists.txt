file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_executable(activation-context-dump ${SRC_FILES})

set_property(TARGET activation-context-dump PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

sogen_assign_source_group(${SRC_FILES})
sogen_target_skip_clang_tidy(activation-context-dump)
