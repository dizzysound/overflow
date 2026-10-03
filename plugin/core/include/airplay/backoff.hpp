// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace airplay {

// Restart delays: each next() returns the following step; the last repeats.
class Backoff {
public:
	explicit Backoff(std::vector<std::chrono::milliseconds> steps);
	std::chrono::milliseconds next();
	void reset();

private:
	std::vector<std::chrono::milliseconds> steps_;
	size_t index_ = 0;
};

// 1 s, 2 s, 5 s, then every 10 s (the spec's reconnect schedule).
std::vector<std::chrono::milliseconds> default_helper_backoff();

} // namespace airplay
