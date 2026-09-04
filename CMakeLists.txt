cmake_minimum_required(VERSION 3.16)

project(vlc_spanning
    VERSION 1.5
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

list(GET LIBVLC_LIBRARY_DIRS 0 LIBVLC_LIBRARY_DIR)
set(LIBVLC_PLUGIN_DIR "${LIBVLC_LIBRARY_DIR}/vlc")
set(LIBVLC_DATA_DIR "/usr/share/vlc")
file(GLOB LIBVLC_RUNTIME_LIBS
    "${LIBVLC_LIBRARY_DIR}/libvlc.so.*"
    "${LIBVLC_LIBRARY_DIR}/libvlccore.so.*"
)
if(NOT LIBVLC_RUNTIME_LIBS OR NOT IS_DIRECTORY "${LIBVLC_PLUGIN_DIR}")
    message(FATAL_ERROR "Could not locate the LibVLC runtime or plugin directory")
endif()

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

set_target_properties(vlc_spanning PROPERTIES
    BUILD_RPATH "${LIBVLC_LIBRARY_DIR}"
    INSTALL_RPATH "$ORIGIN/../lib/vlc-spanning"
)

install(TARGETS vlc_spanning
    RUNTIME DESTINATION bin
)

# Bundle LibVLC so the installed app does not require the VLC package.
install(FILES ${LIBVLC_RUNTIME_LIBS}
    DESTINATION lib/vlc-spanning
)
install(DIRECTORY "${LIBVLC_PLUGIN_DIR}/"
    DESTINATION lib/vlc-spanning/plugins
    FILES_MATCHING
        PATTERN "*.so*"
        PATTERN "plugins.dat"
)
install(DIRECTORY "${LIBVLC_DATA_DIR}/"
    DESTINATION share/vlc-spanning
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
set(CPACK_DEBIAN_PACKAGE_DEPENDS "yt-dlp, libsdl2-2.0-0, libgtk-4-1, libx11-6")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

include(CPack)
