cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

if(NOT ENABLE_FRONTEND_API OR NOT ENABLE_QT)
  message(FATAL_ERROR "Audio Stems Recorder requires ENABLE_FRONTEND_API=ON and ENABLE_QT=ON")
endif()

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)

if(WIN32)
  # Keep <windows.h> from defining min/max macros that clash with std::min/std::max.
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/stems/finalizer.cpp
    src/stems/finalizer.hpp
    src/stems/paths.cpp
    src/stems/paths.hpp
    src/stems/session.cpp
    src/stems/session.hpp
    src/stems/settings.cpp
    src/stems/settings.hpp
    src/stems/settings_dialog.cpp
    src/stems/settings_dialog.hpp
    src/stems/spsc_ring.hpp
    src/stems/stem_plugin.cpp
    src/stems/stem_plugin.hpp
    src/stems/stem_recorder.cpp
    src/stems/stem_recorder.hpp
    src/stems/stems_dock.cpp
    src/stems/stems_dock.hpp
    src/stems/timeline.hpp
    src/stems/transcode.cpp
    src/stems/transcode.hpp
    src/stems/wav_io.cpp
    src/stems/wav_io.hpp
    src/stems/wav_postprocess.cpp
    src/stems/wav_postprocess.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# OBS 32+ plugin manager metadata (data/manifest.json), versioned from buildspec.json
configure_file(cmake/common/resources/manifest.json.in "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" @ONLY)
target_add_resource(${CMAKE_PROJECT_NAME} "${CMAKE_CURRENT_BINARY_DIR}/manifest.json")
