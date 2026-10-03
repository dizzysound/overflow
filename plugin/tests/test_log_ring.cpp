// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/log_ring.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace airplay;

TEST_CASE("empty ring holds nothing")
{
	LogRing ring;
	CHECK(ring.size() == 0);
	CHECK(ring.lines().empty());
}

TEST_CASE("lines come back in push order, with their timestamps, while under capacity")
{
	LogRing ring;
	ring.push(1, "first");
	ring.push(2, "second");
	ring.push(3, "third");
	const std::vector<LogLine> lines = ring.lines();
	REQUIRE(lines.size() == 3);
	CHECK(lines[0].timestamp_ns == 1);
	CHECK(lines[0].text == "first");
	CHECK(lines[1].text == "second");
	CHECK(lines[2].text == "third");
}

TEST_CASE("pushing past capacity keeps only the most recent kCapacity lines, oldest first")
{
	LogRing ring;
	const size_t total = LogRing::kCapacity + 10;
	for (size_t i = 0; i < total; ++i)
		ring.push(static_cast<uint64_t>(i), "line " + std::to_string(i));
	CHECK(ring.size() == LogRing::kCapacity);
	const std::vector<LogLine> lines = ring.lines();
	REQUIRE(lines.size() == LogRing::kCapacity);
	// The oldest surviving line is number 10 (0..9 were evicted).
	CHECK(lines.front().text == "line 10");
	CHECK(lines.back().text == "line " + std::to_string(total - 1));
}

TEST_CASE("concurrent pushes from several threads never crash or lose the count past capacity")
{
	LogRing ring;
	constexpr int kThreads = 8;
	constexpr int kPerThread = 200;
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&ring, t, kPerThread] {
			for (int i = 0; i < kPerThread; ++i)
				ring.push(static_cast<uint64_t>(t * kPerThread + i), "t" + std::to_string(t));
		});
	}
	for (std::thread &th : threads)
		th.join();
	CHECK(ring.size() == static_cast<size_t>(kThreads * kPerThread));
	CHECK(ring.lines().size() == ring.size());
}
