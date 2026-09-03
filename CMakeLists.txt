cmake_minimum_required(VERSION 3.16)

project(vlc_spanning
    VERSION 1.3
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(X11 REQUIRED)

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(LIBVLC REQUIRED IMPORTED_TARGET libvlc)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

add_executable(vlc_spanning
    src/main.cpp
    src/media_controls.cpp
)

target_link_libraries(vlc_spanning
    PRIVATE
        PkgConfig::SDL2
        PkgConfig::LIBVLC
        PkgConfig::GTK4
        X11::X11
)

target_compile_options(vlc_spanning
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

install(TARGETS vlc_spanning
    RUNTIME DESTINATION bin
)

install(FILES resources/com.noirshake.VLCSpanning.desktop
    DESTINATION share/applications
)

install(FILES resources/com.noirshake.VLCSpanning.svg
    DESTINATION share/icons/hicolor/scalable/apps
)

install(FILES README.md
    DESTINATION share/doc/vlc-spanning
)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "vlc-spanning")
set(CPACK_PACKAGE_VENDOR "Noirshake")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Dual-monitor LibVLC media player")
set(CPACK_PACKAGE_DESCRIPTION "VLC Spanning Player provides one LibVLC video canvas across a multi-monitor X11/XWayland desktop.")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Noirshake")
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

include(CPack)
