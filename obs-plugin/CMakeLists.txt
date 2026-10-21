cmake_minimum_required(VERSION 3.24)
project(livedelay-obs VERSION 1.0.0 LANGUAGES CXX)

# Built against OBS 31 headers and import libraries generated from the installed
# obs.dll / obs-frontend-api.dll (see ../../deps), plus OBS's own prebuilt Qt 6.6.3.
set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../../deps" CACHE PATH "obs-studio sources, Qt and import libs")
list(APPEND CMAKE_PREFIX_PATH "${DEPS}/qt6")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
set(CMAKE_AUTOMOC ON)

# The proxy itself: the Rust crate one folder up, built as a static library (cargo build --release).
set(RUST_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../target/release/livedelay.lib")
if(NOT EXISTS "${RUST_LIB}")
  message(FATAL_ERROR "Brak ${RUST_LIB} - najpierw: cargo build --release")
endif()
# Import library the Rust windows-sys bindings link against (shipped in the cargo registry).
file(GLOB WINDOWS_TARGETS_LIB "$ENV{USERPROFILE}/.cargo/registry/src/*/windows_x86_64_msvc-0.52.*/lib/windows.0.52.0.lib")
list(GET WINDOWS_TARGETS_LIB 0 WINDOWS_TARGETS_LIB)

add_library(livedelay-obs MODULE
  src/plugin.cpp src/dock.cpp src/client.cpp src/settings.cpp src/services.cpp
  src/icons.cpp src/limits.cpp src/encode.cpp src/outputs.cpp src/update.cpp src/update.hpp src/version.rc)
set_target_properties(livedelay-obs PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(livedelay-obs PRIVATE /utf-8 /W3 /Zc:__cplusplus /permissive-)
target_compile_definitions(livedelay-obs PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(livedelay-obs PRIVATE
  "${DEPS}/obslibs/include"
  "${DEPS}/obs-studio/libobs"
  "${DEPS}/obs-studio/UI/obs-frontend-api")
target_link_libraries(livedelay-obs PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets
  "${DEPS}/obslibs/obs.lib"
  "${DEPS}/obslibs/obs-frontend-api.lib"
  "${RUST_LIB}" "${WINDOWS_TARGETS_LIB}"
  ws2_32 userenv ntdll dbghelp bcrypt winhttp)
