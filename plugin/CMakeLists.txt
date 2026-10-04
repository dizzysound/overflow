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

# overflow-helper and eld-encoder ship beside the plugin binary, where
# plugin-main.cpp looks for them: bin/64bit on Windows, Contents/MacOS in the
# macOS bundle, and next to obs-overflow.so on Linux. CI and the local build
# scripts build them (see .github/workflows/plugin.yml) and pass their
# directory here.
set(AIRPLAY_HELPER_BIN_DIR "" CACHE PATH "Directory with overflow-helper, eld-encoder and eld-encoder-FDK-AAC-NOTICE.txt")
if(AIRPLAY_HELPER_BIN_DIR)
  if(OS_WINDOWS)
    set(_airplay_helper_programs overflow-helper.exe eld-encoder.exe)
  else()
    set(_airplay_helper_programs overflow-helper eld-encoder)
  endif()
  set(_airplay_helper_paths "")
  foreach(_file IN LISTS _airplay_helper_programs ITEMS eld-encoder-FDK-AAC-NOTICE.txt)
    if(NOT EXISTS "${AIRPLAY_HELPER_BIN_DIR}/${_file}")
      message(FATAL_ERROR "AIRPLAY_HELPER_BIN_DIR (${AIRPLAY_HELPER_BIN_DIR}) has no ${_file}")
    endif()
    list(APPEND _airplay_helper_paths "${AIRPLAY_HELPER_BIN_DIR}/${_file}")
  endforeach()
  if(OS_WINDOWS)
    install(PROGRAMS ${_airplay_helper_paths} DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit")
  elseif(OS_MACOS)
    # Copied into the bundle at build time. The programs must already be
    # signed (ad hoc is enough) so the bundle's signature can seal them, and
    # Contents/MacOS may hold only code, so the notice goes to Resources.
    foreach(_path IN LISTS _airplay_helper_paths)
      target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${_path}")
      if(_path MATCHES "\\.txt$")
        set_property(SOURCE "${_path}" PROPERTY MACOSX_PACKAGE_LOCATION Resources/licenses)
      else()
        set_property(SOURCE "${_path}" PROPERTY MACOSX_PACKAGE_LOCATION MacOS)
      endif()
    endforeach()
  else()
    list(FILTER _airplay_helper_paths EXCLUDE REGEX "\\.txt$")
    install(PROGRAMS ${_airplay_helper_paths} DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
    install(FILES "${AIRPLAY_HELPER_BIN_DIR}/eld-encoder-FDK-AAC-NOTICE.txt" DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins")
  endif()
endif()

# Licenses and the README travel with the package: beside bin/ on Windows, in
# the bundle's Resources on macOS, in the plugin's data directory on Linux.
set(_airplay_docs
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE|LICENSE.txt|"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md|README.txt|"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/third_party/nlohmann/LICENSE.MIT|nlohmann-json-MIT.txt|licenses"
    "${CMAKE_CURRENT_SOURCE_DIR}/../helper/LICENSE|doubletake-LGPL-3.0.txt|licenses"
    "${CMAKE_CURRENT_SOURCE_DIR}/../eld-encoder/LICENSE-NOTICE.md|eld-encoder-LICENSE-NOTICE.md|licenses"
)
foreach(_doc IN LISTS _airplay_docs)
  string(REPLACE "|" ";" _doc "${_doc}")
  list(GET _doc 0 _src)
  list(GET _doc 1 _name)
  list(LENGTH _doc _len)
  set(_sub "")
  if(_len GREATER 2)
    list(GET _doc 2 _sub)
  endif()
  if(OS_WINDOWS)
    install(FILES "${_src}" DESTINATION "${CMAKE_PROJECT_NAME}/${_sub}" RENAME "${_name}")
  elseif(OS_MACOS)
    # Bundle resources keep their source file name, so copy under the
    # shipped name first.
    set(_staged "${CMAKE_CURRENT_BINARY_DIR}/bundle-docs/${_name}")
    configure_file("${_src}" "${_staged}" COPYONLY)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE "${_staged}")
    set_property(SOURCE "${_staged}" PROPERTY MACOSX_PACKAGE_LOCATION "Resources/${_sub}")
  else()
    install(
      FILES "${_src}"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/obs/obs-plugins/${CMAKE_PROJECT_NAME}/${_sub}"
      RENAME "${_name}"
    )
  endif()
endforeach()
