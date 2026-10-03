# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Christopher Gillespie

cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# obs-overflow always uses the frontend API (dock, events) and Qt (widgets).
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp)

add_subdirectory(core)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE airplay-core)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/airplay-output.cpp src/airplay-output.hpp)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/controller.cpp src/controller.hpp src/ui/dock-backend.hpp)
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/ui/airplay-dock.cpp
    src/ui/airplay-dock.hpp
    src/ui/display-settings-dialog.cpp
    src/ui/display-settings-dialog.hpp
    src/ui/global-settings-dialog.cpp
    src/ui/global-settings-dialog.hpp
    src/ui/credential-dialog.cpp
    src/ui/credential-dialog.hpp
    src/ui/add-display-dialog.cpp
    src/ui/add-display-dialog.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# overflow-helper.exe and eld-encoder.exe ship beside obs-overflow.dll. CI builds
# them (see .github/workflows/plugin.yml) and passes their directory here.
set(AIRPLAY_HELPER_BIN_DIR "" CACHE PATH "Directory with overflow-helper.exe, eld-encoder.exe and eld-encoder-FDK-AAC-NOTICE.txt")
if(OS_WINDOWS AND AIRPLAY_HELPER_BIN_DIR)
  set(_airplay_helper_files overflow-helper.exe eld-encoder.exe eld-encoder-FDK-AAC-NOTICE.txt)
  set(_airplay_helper_paths "")
  foreach(_file IN LISTS _airplay_helper_files)
    if(NOT EXISTS "${AIRPLAY_HELPER_BIN_DIR}/${_file}")
      message(FATAL_ERROR "AIRPLAY_HELPER_BIN_DIR (${AIRPLAY_HELPER_BIN_DIR}) has no ${_file}")
    endif()
    list(APPEND _airplay_helper_paths "${AIRPLAY_HELPER_BIN_DIR}/${_file}")
  endforeach()
  install(PROGRAMS ${_airplay_helper_paths} DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit")
endif()

# Licenses and the README travel with the Windows package.
if(OS_WINDOWS)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_PROJECT_NAME}" RENAME LICENSE.txt)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION "${CMAKE_PROJECT_NAME}" RENAME README.txt)
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/core/third_party/nlohmann/LICENSE.MIT"
    DESTINATION "${CMAKE_PROJECT_NAME}/licenses"
    RENAME nlohmann-json-MIT.txt
  )
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../helper/LICENSE"
    DESTINATION "${CMAKE_PROJECT_NAME}/licenses"
    RENAME doubletake-LGPL-3.0.txt
  )
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../eld-encoder/LICENSE-NOTICE.md"
    DESTINATION "${CMAKE_PROJECT_NAME}/licenses"
    RENAME eld-encoder-LICENSE-NOTICE.md
  )
endif()
