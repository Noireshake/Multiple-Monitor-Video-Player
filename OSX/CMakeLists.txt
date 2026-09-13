cmake_minimum_required(VERSION 3.20)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.11" CACHE STRING "Minimum macOS version")

project(vlc_spanning_macos VERSION 1.6 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

set(VLC_BUNDLE_ROOT "${CMAKE_INSTALL_PREFIX}/VLC Spanning Player.app/Contents")

set(VLC_APP "/Applications/VLC.app/Contents/MacOS")
set(VLC_INCLUDE_DIR "${VLC_APP}/include")
set(VLC_LIBRARY_DIR "${VLC_APP}/lib")
set(VLC_PLUGIN_DIR "${VLC_APP}/plugins")
set(VLC_DATA_DIR "${VLC_APP}/share")

if(NOT EXISTS "${VLC_INCLUDE_DIR}/vlc/vlc.h" OR NOT EXISTS "${VLC_LIBRARY_DIR}/libvlc.dylib")
    message(FATAL_ERROR "LibVLC development files were not found under ${VLC_APP}")
endif()

add_executable(vlc_spanning_macos MACOSX_BUNDLE main.mm)
target_include_directories(vlc_spanning_macos PRIVATE "${VLC_INCLUDE_DIR}")
target_link_directories(vlc_spanning_macos PRIVATE "${VLC_LIBRARY_DIR}")
target_link_libraries(vlc_spanning_macos PRIVATE vlc "-framework Cocoa" "-framework CoreGraphics")
target_compile_options(vlc_spanning_macos PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(vlc_spanning_macos PRIVATE "-Wl,-rpath,@executable_path/../Frameworks")
set_target_properties(vlc_spanning_macos PROPERTIES
    BUILD_RPATH "${VLC_LIBRARY_DIR}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.noirshake.VLCSpanning"
    MACOSX_BUNDLE_BUNDLE_NAME "VLC Spanning Player"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "VLCSpanning.icns"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
)

install(TARGETS vlc_spanning_macos BUNDLE DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/VLCSpanning.icns"
    DESTINATION "vlc_spanning_macos.app/Contents/Resources")

set(CPACK_GENERATOR DragNDrop)
set(CPACK_PACKAGE_NAME "vlc-spanning")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
include(CPack)

message(STATUS "macOS LibVLC: ${VLC_APP}")
message(STATUS "macOS architecture: ${CMAKE_OSX_ARCHITECTURES}")
