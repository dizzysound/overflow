// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/keyframe_gate.hpp"

namespace airplay {

KeyframeGate::Decision KeyframeGate::request(uint64_t now_ns)
{
	Decision d;
	if (any_ && now_ns - last_ns_ < min_interval_ns_) {
		d.defer_until_ns = last_ns_ + min_interval_ns_;
		return d;
	}
	any_ = true;
	last_ns_ = now_ns;
	d.send_now = true;
	return d;
}

} // namespace airplay
