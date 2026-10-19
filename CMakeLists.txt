cmake_minimum_required(VERSION 3.10)
project(fcitx4-gtk4)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(FCITX REQUIRED fcitx)
pkg_check_modules(FCITX_GCLIENT REQUIRED fcitx-gclient)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

set(LOCALEDIR "${CMAKE_INSTALL_PREFIX}/share/locale")
configure_file(config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

add_library(im-fcitx4 MODULE src/fcitxim.c src/fcitximcontext.c)
set_target_properties(im-fcitx4 PROPERTIES PREFIX "lib")
target_include_directories(im-fcitx4 PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${GTK4_INCLUDE_DIRS}
    ${FCITX_INCLUDE_DIRS}
    ${FCITX_GCLIENT_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
)
target_compile_definitions(im-fcitx4 PRIVATE
    GTK4=1
    ENABLE_SNOOPER=0
    "NO_SNOOPER_APPS=\"\""
    G_LOG_DOMAIN="Fcitx"
    ${GTK4_DEFINITIONS}
)
target_link_libraries(im-fcitx4 PRIVATE
    ${GTK4_LIBRARIES}
    ${FCITX_LIBRARIES}
    ${FCITX_GCLIENT_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    X11
)
target_link_directories(im-fcitx4 PRIVATE
    ${GTK4_LIBRARY_DIRS}
    ${FCITX_LIBRARY_DIRS}
    ${FCITX_GCLIENT_LIBRARY_DIRS}
    ${XKBCOMMON_LIBRARY_DIRS}
)
set_target_properties(im-fcitx4 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/immodules"
)

install(TARGETS im-fcitx4 DESTINATION lib/gtk-4.0/4.0.0/immodules)