// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Polls pred every 5 ms until it is true or timeout passes.
inline bool wait_until(const std::function<bool()> &pred,
		       std::chrono::milliseconds timeout = std::chrono::milliseconds(10000))
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		if (pred())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return pred();
}

inline uint64_t test_now_ns()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					     std::chrono::steady_clock::now().time_since_epoch())
					     .count());
}

// A thread-safe list of lines.
class Lines {
public:
	void add(std::string line)
	{
		std::lock_guard<std::mutex> lock(mu_);
		lines_.push_back(std::move(line));
	}
	std::vector<std::string> snapshot() const
	{
		std::lock_guard<std::mutex> lock(mu_);
		return lines_;
	}
	size_t count_containing(const std::string &needle) const
	{
		std::lock_guard<std::mutex> lock(mu_);
		size_t n = 0;
		for (const std::string &l : lines_)
			if (l.find(needle) != std::string::npos)
				++n;
		return n;
	}
	bool any_contains(const std::string &needle) const { return count_containing(needle) > 0; }
	size_t size() const
	{
		std::lock_guard<std::mutex> lock(mu_);
		return lines_.size();
	}

private:
	mutable std::mutex mu_;
	std::vector<std::string> lines_;
};
