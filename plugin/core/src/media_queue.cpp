// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/media_queue.hpp"

#include <algorithm>
#include <utility>

namespace airplay {

MediaQueue::MediaQueue(MediaQueueLimits limits) : limits_(limits) {}

void MediaQueue::push_video(std::vector<uint8_t> access_unit, uint64_t capture_ns, bool keyframe)
{
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (waiting_for_keyframe_ && !keyframe) {
			++stats_.video_dropped;
			return;
		}
		if (video_bytes_ + access_unit.size() > limits_.max_video_bytes) {
			// A keyframe is a clean restart point, so make room for it.
			if (keyframe)
				purge_video_locked();
			if (video_bytes_ + access_unit.size() > limits_.max_video_bytes) {
				++stats_.video_dropped;
				waiting_for_keyframe_ = true;
				return;
			}
		}
		waiting_for_keyframe_ = false;
		video_bytes_ += access_unit.size();
		items_.push_back(MediaItem{MsgType::VideoAU, capture_ns, keyframe, std::move(access_unit)});
	}
	cv_.notify_one();
}

void MediaQueue::push_audio(std::vector<uint8_t> pcm, uint64_t capture_ns)
{
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (pcm.size() > limits_.max_audio_bytes) {
			++stats_.audio_dropped;
			return;
		}
		while (audio_bytes_ + pcm.size() > limits_.max_audio_bytes) {
			const auto oldest = std::find_if(items_.begin(), items_.end(),
							 [](const MediaItem &m) { return m.type == MsgType::AudioPCM; });
			if (oldest == items_.end())
				break;
			audio_bytes_ -= oldest->data.size();
			items_.erase(oldest);
			++stats_.audio_dropped;
		}
		audio_bytes_ += pcm.size();
		items_.push_back(MediaItem{MsgType::AudioPCM, capture_ns, false, std::move(pcm)});
	}
	cv_.notify_one();
}

std::optional<MediaItem> MediaQueue::pop(std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(mu_);
	cv_.wait_for(lock, timeout, [&] { return !items_.empty() || wake_; });
	if (items_.empty()) {
		// Nothing to hand back: consume the wake so a stale notify() does
		// not spin the next pop().
		wake_ = false;
		return std::nullopt;
	}
	// An item is available; leave wake_ set if it was set, so a pending
	// notify() also lets the following pop() return promptly.
	MediaItem item = std::move(items_.front());
	items_.pop_front();
	if (item.type == MsgType::VideoAU)
		video_bytes_ -= item.data.size();
	else
		audio_bytes_ -= item.data.size();
	return item;
}

void MediaQueue::notify()
{
	{
		std::lock_guard<std::mutex> lock(mu_);
		wake_ = true;
	}
	cv_.notify_all();
}

void MediaQueue::reset()
{
	std::lock_guard<std::mutex> lock(mu_);
	items_.clear();
	video_bytes_ = 0;
	audio_bytes_ = 0;
	waiting_for_keyframe_ = true;
	wake_ = false;
}

void MediaQueue::drop_video_until_keyframe()
{
	std::lock_guard<std::mutex> lock(mu_);
	purge_video_locked();
	waiting_for_keyframe_ = true;
}

void MediaQueue::purge_video_locked()
{
	// A single pass that keeps only non-video items, rather than erasing
	// from the middle of the deque one element at a time: that would be
	// quadratic under the producer lock on OBS's encoder thread.
	std::deque<MediaItem> kept;
	for (auto &item : items_) {
		if (item.type == MsgType::VideoAU) {
			video_bytes_ -= item.data.size();
			++stats_.video_dropped;
		} else {
			kept.push_back(std::move(item));
		}
	}
	items_ = std::move(kept);
}

MediaQueueStats MediaQueue::stats() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return stats_;
}

size_t MediaQueue::size() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return items_.size();
}

} // namespace airplay
