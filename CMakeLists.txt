# Pragmas
cmake_minimum_required(VERSION 3.20)

project(otakushell VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_definitions(OTAKU_VERSION="${PROJECT_VERSION}")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
find_package(tomlplusplus REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-scanner)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo pangocairo)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

# ---------------------------------------------------------------------------
# Wayland protocol stubs (generated)
# ---------------------------------------------------------------------------
find_program(wayland_scanner_executable NAMES wayland-scanner REQUIRED)

set(PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocol)
set(PROTOCOL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/protocol-generated)

function(generate_wayland_protocol proto_name xml_name)
  add_custom_command(
    OUTPUT ${PROTOCOL_GEN_DIR}/${proto_name}-protocol.c
           ${PROTOCOL_GEN_DIR}/${proto_name}-client-protocol.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTOCOL_GEN_DIR}
    COMMAND ${wayland_scanner_executable} private-code
            ${PROTOCOL_DIR}/${xml_name}.xml ${PROTOCOL_GEN_DIR}/${proto_name}-protocol.c
    COMMAND ${wayland_scanner_executable} client-header
            ${PROTOCOL_DIR}/${xml_name}.xml ${PROTOCOL_GEN_DIR}/${proto_name}-client-protocol.h
    # `namespace` is a C++ keyword; rename it so the header compiles as C++.
    COMMAND sed -i 's/\\bnamespace\\b/namespace_/g'
            ${PROTOCOL_GEN_DIR}/${proto_name}-client-protocol.h
    COMMAND sed -i 's/\\bnamespace\\b/namespace_/g'
            ${PROTOCOL_GEN_DIR}/${proto_name}-protocol.c
    DEPENDS ${PROTOCOL_DIR}/${xml_name}.xml
    COMMENT "Generating Wayland protocol stubs: ${proto_name}"
  )
endfunction()

generate_wayland_protocol(wlr-layer-shell wlr-layer-shell-unstable-v1)
generate_wayland_protocol(xdg-shell xdg-shell)
generate_wayland_protocol(xdg-output xdg-output-unstable-v1)
generate_wayland_protocol(ext-session-lock ext-session-lock-v1)

# ---------------------------------------------------------------------------
# Core engine library (shared by daemon and CLI)
# ---------------------------------------------------------------------------
add_library(otaku_core STATIC
  src/config.cpp
  src/wayland.cpp
  src/render.cpp
  src/shm.cpp
  src/frame.cpp
  src/module.cpp
  src/hypr.cpp
  ${PROTOCOL_GEN_DIR}/wlr-layer-shell-protocol.c
  ${PROTOCOL_GEN_DIR}/xdg-shell-protocol.c
  ${PROTOCOL_GEN_DIR}/xdg-output-protocol.c
  ${PROTOCOL_GEN_DIR}/ext-session-lock-protocol.c
)
target_include_directories(otaku_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${PROTOCOL_GEN_DIR}
)
target_link_libraries(otaku_core PUBLIC
  PkgConfig::WAYLAND
  PkgConfig::CAIRO
  PkgConfig::DBUS
  tomlplusplus::tomlplusplus
)
target_compile_options(otaku_core PUBLIC -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# Daemon: otakud
# ---------------------------------------------------------------------------
add_executable(otakud src/daemon.cpp)
target_link_libraries(otakud PRIVATE otaku_core)
target_link_options(otakud PRIVATE -rdynamic)

# ---------------------------------------------------------------------------
# CLI: otakushell
# ---------------------------------------------------------------------------
add_executable(otakushell src/cli.cpp)
target_link_libraries(otakushell PRIVATE otaku_core)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS otakud otakushell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
