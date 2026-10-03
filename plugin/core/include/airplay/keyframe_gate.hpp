// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Throttles forced keyframes: each one resets obs-nvenc (a large IDR), so a
// burst of helper requests must not become a burst of IDRs on the network. A
// request inside the window is deferred to the window's end, never dropped: a
// dropped request can leave a display frozen until the next scheduled IDR.
#pragma once

#include <cstdint>

namespace airplay {

// 200 ms, below the helper's 250 ms throttle, so the helper's trailing request
// is never deferred on scheduling jitter alone.
constexpr uint64_t kKeyframeGateNs = 200ull * 1000 * 1000;

class KeyframeGate {
public:
	struct Decision {
		bool send_now = false;
		// When !send_now: call request() again at this time. Every request in
		// one window gets the same time, so the caller arms one timer.
		uint64_t defer_until_ns = 0;
	};

	explicit KeyframeGate(uint64_t min_interval_ns) : min_interval_ns_(min_interval_ns) {}
	Decision request(uint64_t now_ns);

private:
	uint64_t min_interval_ns_;
	uint64_t last_ns_ = 0;
	bool any_ = false;
};

} // namespace airplay
