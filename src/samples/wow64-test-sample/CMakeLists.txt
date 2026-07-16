file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_executable(wow64-test-sample ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})
