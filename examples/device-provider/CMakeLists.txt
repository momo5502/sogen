add_executable(device-provider-example EXCLUDE_FROM_ALL main.cpp)
target_compile_features(device-provider-example PRIVATE cxx_std_20)
target_link_libraries(device-provider-example PRIVATE windows-emulator backend-selection)
