add_library(qt-raster-protocol INTERFACE)
target_include_directories(qt-raster-protocol INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
target_sources(qt-raster-protocol INTERFACE qt_raster_protocol.hpp)

add_library(qt-raster-session qt_raster_session.cpp)
target_link_libraries(qt-raster-session PUBLIC qt-raster-protocol)
