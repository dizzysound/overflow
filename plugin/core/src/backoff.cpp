// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/backoff.hpp"

#include <utility>

namespace airplay {

Backoff::Backoff(std::vector<std::chrono::milliseconds> steps) : steps_(std::move(steps)) {}

std::chrono::milliseconds Backoff::next()
{
	if (steps_.empty())
		return std::chrono::seconds(1);
	const std::chrono::milliseconds d = steps_[index_ < steps_.size() ? index_ : steps_.size() - 1];
	if (index_ < steps_.size())
		++index_;
	return d;
}

void Backoff::reset()
{
	index_ = 0;
}

std::vector<std::chrono::milliseconds> default_helper_backoff()
{
	return {std::chrono::seconds(1), std::chrono::seconds(2), std::chrono::seconds(5), std::chrono::seconds(10)};
}

} // namespace airplay
