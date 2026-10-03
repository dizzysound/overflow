// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/latency_policy.hpp"

#include <doctest/doctest.h>

using namespace airplay;

TEST_CASE("auto_latency_ms: zero age gives the 150 ms margin")
{
	CHECK(auto_latency_ms(0) == 150);
}

TEST_CASE("auto_latency_ms: 985 ms age rounds up to 1150")
{
	CHECK(auto_latency_ms(985ull * 1000000ull) == 1150);
}

TEST_CASE("auto_latency_ms: exact step boundaries hold")
{
	// age 0 -> raw 150, already a step: 150.
	CHECK(auto_latency_ms(0) == 150);
	// age 50 ms -> raw 200, already a step: 200.
	CHECK(auto_latency_ms(50ull * 1000000ull) == 200);
	// age 1 ms -> raw 151, rounds up to 200.
	CHECK(auto_latency_ms(1ull * 1000000ull) == 200);
}

TEST_CASE("auto_latency_ms: clamps to the [100, 2000] range")
{
	CHECK(auto_latency_ms(0) >= 100);
	// A huge age clamps to the 2000 ms ceiling rather than overflowing.
	CHECK(auto_latency_ms(1000ull * 1000000000ull) == 2000);
	CHECK(auto_latency_ms(UINT64_MAX) == 2000);
}

TEST_CASE("should_raise: unset current (0) raises for any positive wanted value")
{
	CHECK(should_raise(0, 150));
	CHECK_FALSE(should_raise(0, 0));
}

TEST_CASE("should_raise: only raises when wanted exceeds current by more than 100 ms")
{
	CHECK_FALSE(should_raise(500, 500));
	CHECK_FALSE(should_raise(500, 600)); // exactly +100: not worth a restart
	CHECK(should_raise(500, 601));
	CHECK(should_raise(500, 1150));
	CHECK_FALSE(should_raise(1150, 1000)); // never lowers
}
