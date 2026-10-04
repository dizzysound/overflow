# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Christopher Gillespie

# Standalone tests for airplay-core. No OBS, no Qt:
#   cmake -S plugin/tests -B plugin/build-tests
#   cmake --build plugin/build-tests --config Debug
#   ctest --test-dir plugin/build-tests -C Debug --output-on-failure
cmake_minimum_required(VERSION 3.24)
project(airplay-core-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../core core)

enable_testing()

add_executable(core-tests test_main.cpp)
target_include_directories(core-tests SYSTEM PRIVATE third_party)
target_link_libraries(core-tests PRIVATE airplay-core)
add_test(NAME core-tests COMMAND core-tests)
set_tests_properties(core-tests PROPERTIES TIMEOUT 300)

target_sources(core-tests PRIVATE test_frame.cpp)
target_sources(core-tests PRIVATE test_annexb.cpp)
target_sources(core-tests PRIVATE test_commands.cpp)
target_sources(core-tests PRIVATE test_events.cpp)
target_sources(core-tests PRIVATE test_settings.cpp)
target_sources(core-tests PRIVATE test_config_migration.cpp)
target_sources(core-tests PRIVATE test_log_ring.cpp)
target_sources(core-tests PRIVATE test_redact.cpp)
target_sources(core-tests PRIVATE test_secret.cpp)
target_sources(core-tests PRIVATE test_encoder_policy.cpp)
target_sources(core-tests PRIVATE test_session_policy.cpp)
target_sources(core-tests PRIVATE test_latency_policy.cpp)
target_sources(core-tests PRIVATE test_keyframe_gate.cpp)
target_sources(core-tests PRIVATE test_lead_policy.cpp)
target_sources(core-tests PRIVATE test_live_lead.cpp)
target_sources(core-tests PRIVATE test_ceiling_guard.cpp)
target_sources(core-tests PRIVATE test_run_leads.cpp)
target_sources(core-tests PRIVATE test_viewmodel.cpp)
target_sources(core-tests PRIVATE test_media_queue.cpp)
target_sources(core-tests PRIVATE test_supervisor.cpp)

add_executable(stub-helper stub_helper.cpp)
target_include_directories(stub-helper SYSTEM PRIVATE ../core/third_party)
add_dependencies(core-tests stub-helper)
target_compile_definitions(core-tests PRIVATE STUB_HELPER_PATH="$<TARGET_FILE:stub-helper>")
target_sources(core-tests PRIVATE test_process.cpp)

# Offscreen smoke tests for the dock and dialogs (needs Qt 6):
#   cmake -S plugin/tests -B plugin/build-tests -DAIRPLAY_UI_TESTS=ON -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
option(AIRPLAY_UI_TESTS "Build the Qt dock smoke tests" OFF)
if(AIRPLAY_UI_TESTS)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
  add_executable(
    ui-tests
    ui_test_main.cpp
    test_dock.cpp
    ../src/ui/airplay-dock.cpp
    ../src/ui/display-settings-dialog.cpp
    ../src/ui/global-settings-dialog.cpp
    ../src/ui/credential-dialog.cpp
    ../src/ui/add-display-dialog.cpp
  )
  target_include_directories(ui-tests PRIVATE ../src/ui)
  target_include_directories(ui-tests SYSTEM PRIVATE third_party)
  target_link_libraries(ui-tests PRIVATE airplay-core Qt6::Widgets Qt6::Network)
  add_test(NAME ui-tests COMMAND ui-tests)
  set_tests_properties(ui-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endif()

# Renders the dock and dialogs with sample rooms to PNG for the README (needs Qt 6):
#   cmake -S plugin/tests -B build-shots -DAIRPLAY_SCREENSHOTS=ON -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
option(AIRPLAY_SCREENSHOTS "Build the README screenshot renderer" OFF)
if(AIRPLAY_SCREENSHOTS)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
  add_executable(
    screenshots
    ../tools/screenshots/screenshots.cpp
    ../src/ui/airplay-dock.cpp
    ../src/ui/display-settings-dialog.cpp
    ../src/ui/global-settings-dialog.cpp
    ../src/ui/credential-dialog.cpp
    ../src/ui/add-display-dialog.cpp
  )
  target_include_directories(screenshots PRIVATE ../src/ui)
  target_link_libraries(screenshots PRIVATE airplay-core Qt6::Widgets Qt6::Network)
endif()
