file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_executable(thread-bench ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})
sogen_target_skip_clang_tidy(thread-bench)
