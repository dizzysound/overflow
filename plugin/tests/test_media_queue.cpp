// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/backoff.hpp"
#include "airplay/media_queue.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace airplay;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {
std::vector<uint8_t> bytes(size_t n, uint8_t fill = 0)
{
	return std::vector<uint8_t>(n, fill);
}
} // namespace

TEST_CASE("items come out in push order")
{
	MediaQueue q;
	q.push_video(bytes(10, 1), 100, true);
	q.push_audio(bytes(8, 2), 101);
	q.push_video(bytes(10, 3), 102, false);
	auto a = q.pop(milliseconds(10));
	auto b = q.pop(milliseconds(10));
	auto c = q.pop(milliseconds(10));
	REQUIRE(a);
	REQUIRE(b);
	REQUIRE(c);
	CHECK(a->type == MsgType::VideoAU);
	CHECK(a->keyframe);
	CHECK(a->capture_ns == 100);
	CHECK(b->type == MsgType::AudioPCM);
	CHECK(c->data[0] == 3);
	CHECK_FALSE(q.pop(milliseconds(1)));
}

TEST_CASE("video waits for the first keyframe")
{
	MediaQueue q;
	q.push_video(bytes(10), 1, false);
	CHECK(q.size() == 0);
	CHECK(q.stats().video_dropped == 1);
	q.push_video(bytes(10), 2, true);
	q.push_video(bytes(10), 3, false);
	CHECK(q.size() == 2);
}

TEST_CASE("video overflow drops until the next keyframe, which makes room")
{
	MediaQueue q({100, 1000});
	q.push_video(bytes(60), 1, true);
	q.push_video(bytes(30), 2, false);
	q.push_video(bytes(30), 3, false); // does not fit: dropped, now waiting
	q.push_video(bytes(5), 4, false);  // fits, but we are waiting for a keyframe
	CHECK(q.size() == 2);
	CHECK(q.stats().video_dropped == 2);
	q.push_video(bytes(80), 5, true); // purges the queued video to fit
	CHECK(q.size() == 1);
	auto item = q.pop(milliseconds(1));
	REQUIRE(item);
	CHECK(item->capture_ns == 5);
}

TEST_CASE("audio overflow drops the oldest audio, never video")
{
	MediaQueue q({1000, 16});
	q.push_video(bytes(10), 1, true);
	q.push_audio(bytes(8, 1), 2);
	q.push_audio(bytes(8, 2), 3);
	q.push_audio(bytes(8, 3), 4); // drops the block from t=2
	CHECK(q.stats().audio_dropped == 1);
	auto a = q.pop(milliseconds(1));
	REQUIRE(a);
	CHECK(a->type == MsgType::VideoAU);
	auto b = q.pop(milliseconds(1));
	REQUIRE(b);
	CHECK(b->capture_ns == 3);
	auto c = q.pop(milliseconds(1));
	REQUIRE(c);
	CHECK(c->capture_ns == 4);
}

TEST_CASE("notify wakes a waiting pop early")
{
	MediaQueue q;
	std::thread t([&] {
		std::this_thread::sleep_for(milliseconds(20));
		q.notify();
	});
	const auto start = std::chrono::steady_clock::now();
	CHECK_FALSE(q.pop(milliseconds(5000)));
	CHECK(std::chrono::steady_clock::now() - start < milliseconds(2000));
	t.join();
}

TEST_CASE("notify before pop is not lost")
{
	// notify() is sticky, not edge-triggered: a notify() issued before
	// pop() takes the mutex must still wake that pop() promptly, rather
	// than being missed and forcing a full timeout wait.
	MediaQueue q;
	q.notify();
	const auto start = std::chrono::steady_clock::now();
	CHECK_FALSE(q.pop(milliseconds(5000)));
	CHECK(std::chrono::steady_clock::now() - start < milliseconds(2000));
}

TEST_CASE("reset and drop_video_until_keyframe")
{
	MediaQueue q;
	q.push_video(bytes(10), 1, true);
	q.push_audio(bytes(8), 2);
	q.drop_video_until_keyframe();
	CHECK(q.size() == 1);
	q.push_video(bytes(10), 3, false);
	CHECK(q.size() == 1);
	q.reset();
	CHECK(q.size() == 0);
	q.push_video(bytes(10), 4, false);
	CHECK(q.size() == 0);
}

TEST_CASE("filling to the limit twice does not drift the drop counters")
{
	MediaQueue q({100, 1000});
	q.push_video(bytes(60), 1, true);
	q.push_video(bytes(40), 2, false); // exactly fills to the limit
	CHECK(q.stats().video_dropped == 0);
	CHECK(q.size() == 2);
	while (q.pop(milliseconds(1)))
		;
	CHECK(q.size() == 0);
	q.push_video(bytes(60), 3, true);
	q.push_video(bytes(40), 4, false); // fills to the limit again
	CHECK(q.stats().video_dropped == 0);
	CHECK(q.size() == 2);
}

TEST_CASE("an audio block larger than the limit is dropped on its own")
{
	MediaQueue q({1000, 16});
	q.push_audio(bytes(17), 1);
	CHECK(q.stats().audio_dropped == 1);
	CHECK(q.size() == 0);
}

TEST_CASE("a keyframe larger than the video limit is dropped and we keep waiting")
{
	MediaQueue q({100, 1000});
	q.push_video(bytes(200), 1, true); // too big even alone: dropped
	CHECK(q.stats().video_dropped == 1);
	CHECK(q.size() == 0);
	q.push_video(bytes(10), 2, false); // still waiting for a keyframe
	CHECK(q.stats().video_dropped == 2);
	CHECK(q.size() == 0);
	q.push_video(bytes(10), 3, true); // a keyframe that fits ends the wait
	CHECK(q.size() == 1);
}

TEST_CASE("a producer and a consumer on separate threads")
{
	MediaQueue q;
	std::atomic<int> popped{0};
	std::thread consumer([&] {
		// A generous deadline so a genuine bug reports as a failed CHECK
		// rather than hanging the test.
		const auto deadline = std::chrono::steady_clock::now() + seconds(10);
		while (popped < 1001 && std::chrono::steady_clock::now() < deadline) {
			if (q.pop(milliseconds(100)))
				++popped;
		}
	});
	q.push_video(bytes(100), 1, true);
	for (int i = 0; i < 1000; ++i)
		q.push_audio(bytes(8), static_cast<uint64_t>(i + 2));
	consumer.join();
	CHECK(popped == 1001);
}

TEST_CASE("backoff steps, repeats the last, resets")
{
	Backoff b({milliseconds(1), milliseconds(2), milliseconds(5)});
	CHECK(b.next() == milliseconds(1));
	CHECK(b.next() == milliseconds(2));
	CHECK(b.next() == milliseconds(5));
	CHECK(b.next() == milliseconds(5));
	b.reset();
	CHECK(b.next() == milliseconds(1));
	CHECK(default_helper_backoff().back() == seconds(10));
}

TEST_CASE("default_helper_backoff steps 1s/2s/5s/10s/10s, and resets")
{
	Backoff b(default_helper_backoff());
	CHECK(b.next() == seconds(1));
	CHECK(b.next() == seconds(2));
	CHECK(b.next() == seconds(5));
	CHECK(b.next() == seconds(10));
	CHECK(b.next() == seconds(10)); // repeats the last step
	b.reset();
	CHECK(b.next() == seconds(1));
}

TEST_CASE("an empty Backoff always returns one second")
{
	Backoff b({});
	CHECK(b.next() == seconds(1));
	CHECK(b.next() == seconds(1));
	b.reset();
	CHECK(b.next() == seconds(1));
}
