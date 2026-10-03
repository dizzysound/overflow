// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Pure logic for ruling R7 (automatic TV delay): turning a measured OBS audio
// age into a helper target-latency value, and deciding whether a newly
// measured value is worth raising the current one for.
#pragma once

#include <cstdint>

namespace airplay {

// Rounds (age_ms + 150) up to the next 50 ms step, clamped to [100, 2000].
// The 150 ms margin covers jitter above the worst age seen in the window.
int auto_latency_ms(uint64_t audio_age_ns);

// True when raising the helper's target latency from current_ms to wanted_ms
// is worth a restart: wanted is more than 100 ms above current, or current is
// unset (0) and wanted is positive. Never true for a decrease: TV delay only
// ever rises (OBS's own audio buffering only grows until OBS restarts).
bool should_raise(int current_ms, int wanted_ms);

} // namespace airplay
