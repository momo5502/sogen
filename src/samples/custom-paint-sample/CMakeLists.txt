file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
  *.cpp
  *.hpp
  *.rc
)

list(SORT SRC_FILES)

add_executable(custom-paint-sample ${SRC_FILES})

if(WIN)
  target_link_libraries(custom-paint-sample PRIVATE gdi32 user32)
endif()

sogen_assign_source_group(${SRC_FILES})
