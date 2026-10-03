// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/keyframe_gate.hpp"

#include <doctest/doctest.h>

using namespace airplay;

TEST_CASE("KeyframeGate sends the first request now and defers one inside the window to its end")
{
	constexpr uint64_t kMs = 1000000ull;
	KeyframeGate g(kKeyframeGateNs);
	CHECK(kKeyframeGateNs == 200 * kMs);
	const KeyframeGate::Decision first = g.request(1000 * kMs);
	CHECK(first.send_now);
	const KeyframeGate::Decision second = g.request(1100 * kMs);
	CHECK_FALSE(second.send_now);
	CHECK(second.defer_until_ns == 1200 * kMs);
	// Several requests in one window: still one deferred call, at the same time.
	const KeyframeGate::Decision third = g.request(1150 * kMs);
	CHECK_FALSE(third.send_now);
	CHECK(third.defer_until_ns == 1200 * kMs);
	// The deferred call at the window's end goes out and opens a new window.
	CHECK(g.request(1200 * kMs).send_now);
	const KeyframeGate::Decision fourth = g.request(1250 * kMs);
	CHECK_FALSE(fourth.send_now);
	CHECK(fourth.defer_until_ns == 1400 * kMs);
}

TEST_CASE("KeyframeGate: a deferred call that fires early is deferred again, not dropped")
{
	constexpr uint64_t kMs = 1000000ull;
	KeyframeGate g(kKeyframeGateNs);
	CHECK(g.request(1000 * kMs).send_now);
	CHECK_FALSE(g.request(1050 * kMs).send_now);
	const KeyframeGate::Decision early = g.request(1199 * kMs);
	CHECK_FALSE(early.send_now);
	CHECK(early.defer_until_ns == 1200 * kMs);
	CHECK(g.request(1201 * kMs).send_now);
}

TEST_CASE("KeyframeGate: a request after a quiet window goes out at once")
{
	constexpr uint64_t kMs = 1000000ull;
	KeyframeGate g(kKeyframeGateNs);
	CHECK(g.request(1000 * kMs).send_now);
	CHECK(g.request(1500 * kMs).send_now);
}
