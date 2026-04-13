add_executable(dns-sample main.cpp)

if(WIN)
  target_link_libraries(dns-sample PRIVATE dnsapi ws2_32)
endif()

momo_assign_source_group(main.cpp)
