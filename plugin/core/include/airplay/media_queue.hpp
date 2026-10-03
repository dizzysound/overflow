// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Bounded queue between OBS's encoder/audio threads (producers) and the
// supervisor's writer thread (consumer). Pushing never blocks beyond a short
// mutex. On overflow, video drops until the next keyframe and audio drops its
// oldest blocks, so OBS is never held up by a slow or dead helper.
#pragma once

#include "airplay/frame.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace airplay {

struct MediaQueueLimits {
	size_t max_video_bytes = 4u << 20;  // about 4 s at 8 Mbps
	size_t max_audio_bytes = 512u << 10; // about 3 s of 44.1 kHz stereo S16
};

struct MediaItem {
	MsgType type = MsgType::VideoAU;
	uint64_t capture_ns = 0;
	bool keyframe = false;
	std::vector<uint8_t> data;
};

struct MediaQueueStats {
	uint64_t video_dropped = 0;
	uint64_t audio_dropped = 0;
};

class MediaQueue {
public:
	explicit MediaQueue(MediaQueueLimits limits = {});

	void push_video(std::vector<uint8_t> access_unit, uint64_t capture_ns, bool keyframe);
	void push_audio(std::vector<uint8_t> pcm, uint64_t capture_ns);

	// Waits up to timeout for an item; items come out in push order. Returns
	// nullopt on timeout, or when pop() finds the queue empty after a
	// notify() woke it. notify() is sticky, not edge-triggered: it sets a
	// wake flag under the mutex, so a notify() issued before pop() takes the
	// lock is never lost. If an item is already available when the flag is
	// set, pop() returns that item and leaves the flag set, so the next
	// pop() also returns promptly without a fresh notify(); the flag is
	// cleared only when a pop() call finds the queue empty and returns
	// nullopt.
	std::optional<MediaItem> pop(std::chrono::milliseconds timeout);
	// Sets the sticky wake flag described above and wakes any waiting
	// pop().
	void notify();

	// Drops everything and resets to waiting-for-keyframe: video resumes at
	// the next keyframe. This is a restart, not a drop, so it does not add
	// to stats(). It also clears any pending wake flag set by notify(),
	// since a reset restarts queue state rather than signaling the
	// consumer.
	void reset();
	// Drops queued video; video resumes at the next keyframe.
	void drop_video_until_keyframe();

	MediaQueueStats stats() const;
	size_t size() const;

private:
	void purge_video_locked();

	MediaQueueLimits limits_;
	mutable std::mutex mu_;
	std::condition_variable cv_;
	std::deque<MediaItem> items_;
	size_t video_bytes_ = 0;
	size_t audio_bytes_ = 0;
	bool waiting_for_keyframe_ = true;
	bool wake_ = false;
	MediaQueueStats stats_;
};

} // namespace airplay
