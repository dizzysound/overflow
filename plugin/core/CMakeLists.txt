# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Christopher Gillespie

# airplay-core: the plugin logic that needs neither OBS nor Qt. The plugin
# links it; plugin/tests builds and tests it with plain CMake.
add_library(airplay-core STATIC)
target_compile_features(airplay-core PUBLIC cxx_std_17)
target_include_directories(airplay-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(airplay-core SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set_target_properties(airplay-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(airplay-core PUBLIC Threads::Threads)

# The plugin build treats warnings as errors in CI; fail the same way here.
if(MSVC)
  target_compile_options(airplay-core PRIVATE /W3 /WX /utf-8)
else()
  target_compile_options(
    airplay-core
    PRIVATE -Wall -Wextra -Werror $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wshorten-64-to-32>
  )
endif()

target_sources(airplay-core PRIVATE src/frame.cpp)
target_sources(airplay-core PRIVATE src/annexb.cpp)
target_sources(airplay-core PRIVATE src/commands.cpp)
target_sources(airplay-core PRIVATE src/events.cpp)
target_sources(airplay-core PRIVATE src/settings.cpp)
target_sources(airplay-core PRIVATE src/log_ring.cpp)
target_sources(airplay-core PRIVATE src/redact.cpp)
target_sources(airplay-core PRIVATE src/settings_store.cpp)
target_sources(airplay-core PRIVATE src/config_migration.cpp)
target_sources(airplay-core PRIVATE src/secret.cpp)
target_sources(airplay-core PRIVATE src/encoder_policy.cpp)
target_sources(airplay-core PRIVATE src/session_policy.cpp)
target_sources(airplay-core PRIVATE src/latency_policy.cpp)
target_sources(airplay-core PRIVATE src/keyframe_gate.cpp)
target_sources(airplay-core PRIVATE src/lead_policy.cpp)
target_sources(airplay-core PRIVATE src/live_lead.cpp)
target_sources(airplay-core PRIVATE src/ceiling_guard.cpp)
target_sources(airplay-core PRIVATE src/viewmodel.cpp)
target_sources(airplay-core PRIVATE src/media_queue.cpp)
target_sources(airplay-core PRIVATE src/backoff.cpp)
target_sources(airplay-core PRIVATE src/supervisor.cpp)
target_sources(airplay-core PRIVATE src/process_common.cpp)
if(WIN32)
  target_sources(airplay-core PRIVATE src/process_win.cpp)
else()
  target_sources(airplay-core PRIVATE src/process_posix.cpp)
endif()
if(WIN32)
  target_link_libraries(airplay-core PUBLIC crypt32)
elseif(APPLE)
  target_sources(airplay-core PRIVATE src/secret_macos.cpp)
  target_link_libraries(airplay-core PUBLIC "-framework Security" "-framework CoreFoundation")
else()
  # libsecret is dlopen()ed at run time (secret_linux.cpp), not linked.
  target_sources(airplay-core PRIVATE src/secret_linux.cpp)
  target_link_libraries(airplay-core PUBLIC ${CMAKE_DL_LIBS})
endif()
