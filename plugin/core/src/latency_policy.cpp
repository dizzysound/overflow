// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/latency_policy.hpp"

namespace airplay {
namespace {
constexpr uint64_t kStepMs = 50;
constexpr uint64_t kMarginMs = 150;
constexpr uint64_t kMinMs = 100;
constexpr uint64_t kMaxMs = 2000;
} // namespace

int auto_latency_ms(uint64_t audio_age_ns)
{
	const uint64_t age_ms = audio_age_ns / 1000000ull;
	const uint64_t raw_ms = age_ms + kMarginMs;
	uint64_t rounded_ms = ((raw_ms + kStepMs - 1) / kStepMs) * kStepMs;
	if (rounded_ms < kMinMs)
		rounded_ms = kMinMs;
	if (rounded_ms > kMaxMs)
		rounded_ms = kMaxMs;
	return static_cast<int>(rounded_ms);
}

bool should_raise(int current_ms, int wanted_ms)
{
	if (current_ms == 0)
		return wanted_ms > 0;
	return wanted_ms > current_ms + 100;
}

} // namespace airplay
