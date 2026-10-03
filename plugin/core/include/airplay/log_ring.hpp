// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// A fixed-capacity, thread-safe ring buffer of recent log lines, so the
// operator can save a diagnostic report without reproducing a problem while
// a developer is watching. Fed from helper reader threads (the supervisor's
// on_log callback) and the plugin's own obs_log call sites; read back on the
// UI thread when building the diagnostics report. push() takes a single
// mutex briefly and never blocks for long.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace airplay {

struct LogLine {
	uint64_t timestamp_ns = 0;
	std::string text;
};

class LogRing {
public:
	// The last kCapacity lines are kept; older ones are dropped.
	static constexpr size_t kCapacity = 5000;

	// Appends one line, timestamped now_ns. Thread-safe.
	void push(uint64_t now_ns, const std::string &text);

	// A snapshot of the lines currently held, oldest first. Thread-safe.
	std::vector<LogLine> lines() const;

	size_t size() const;

private:
	mutable std::mutex mu_;
	std::vector<LogLine> lines_;
	size_t next_ = 0; // write cursor once full
	bool full_ = false;
};

} // namespace airplay
