file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
)

list(SORT SRC_FILES)

add_executable(stress-sample ${SRC_FILES})

sogen_assign_source_group(${SRC_FILES})
