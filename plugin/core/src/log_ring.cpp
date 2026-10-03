// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/log_ring.hpp"

namespace airplay {

void LogRing::push(uint64_t now_ns, const std::string &text)
{
	std::lock_guard<std::mutex> lock(mu_);
	if (lines_.size() < kCapacity) {
		lines_.push_back({now_ns, text});
		return;
	}
	full_ = true;
	lines_[next_] = {now_ns, text};
	next_ = (next_ + 1) % kCapacity;
}

std::vector<LogLine> LogRing::lines() const
{
	std::lock_guard<std::mutex> lock(mu_);
	if (!full_)
		return lines_;
	// Oldest first: next_ is the index of the oldest entry once the buffer
	// has wrapped.
	std::vector<LogLine> out;
	out.reserve(lines_.size());
	for (size_t i = 0; i < lines_.size(); ++i)
		out.push_back(lines_[(next_ + i) % lines_.size()]);
	return out;
}

size_t LogRing::size() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return lines_.size();
}

} // namespace airplay
