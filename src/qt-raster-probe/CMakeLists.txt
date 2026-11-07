add_executable(qt-raster-probe main.cpp)
target_link_libraries(qt-raster-probe PRIVATE windows-emulator backend-selection qt-raster-session)
sogen_targets_set_folder("tools" qt-raster-probe)
sogen_strip_target(qt-raster-probe)
