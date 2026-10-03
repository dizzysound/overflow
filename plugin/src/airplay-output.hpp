// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// The "Overflow" OBS output: program video through its own H.264
// encoder (OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED) plus a raw audio tap on one
// mix track. Both only copy into the MediaQueue; the supervisor writes.
#pragma once

#include "airplay/keyframe_gate.hpp"
#include "airplay/media_queue.hpp"
#include "airplay/settings.hpp"

#include <obs.h>

#include <QTimer>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct OutputConfig {
	int audio_track = 1; // OBS track 1-6
	airplay::QualityPreset preset = airplay::QualityPreset::P1080_6Mbps;
	std::string encoder_override;
};

class AirPlayOutput {
public:
	explicit AirPlayOutput(airplay::MediaQueue &queue);
	~AirPlayOutput();
	AirPlayOutput(const AirPlayOutput &) = delete;
	AirPlayOutput &operator=(const AirPlayOutput &) = delete;

	// Registers the output type. Call once from obs_module_load.
	static void register_output_type();
	// H.264 video encoders OBS has registered (not deprecated or internal).
	static std::vector<std::string> available_encoders();
	// The OBS output frame rate, rounded, 1-120.
	static int current_fps();

	// UI thread. Tries each encoder candidate until the output starts.
	// Calling this while active() is a no-op that ignores the new config: to
	// change the preset, encoder override or audio track, the caller must
	// stop() first.
	bool start(const OutputConfig &config, std::string *error);
	void stop();
	bool active() const;
	std::string encoder_id() const { return encoder_id_; }

	// UI thread. Forces an IDR on the shared encoder via obs_encoder_update
	// (obs-nvenc resets with forceIDR on every update). At most one per 200 ms
	// (kKeyframeGateNs): a request inside that window arms one single-shot
	// timer that calls this again at the window's end, so none is lost. False
	// when deferred, inactive, or not an obs-nvenc encoder.
	bool request_keyframe();

	// Called on OBS's encoder and audio threads through the trampolines.
	void on_video_packet(struct encoder_packet *packet);
	void on_audio(struct audio_data *data);

	// Ruling R7: how stale OBS audio is by the time it reaches on_audio(),
	// measured against os_gettime_ns(). The last completed ~2 s window's max
	// age, in nanoseconds; 0 if no window has completed yet (or the tap has
	// never seen audio). Safe to call from the UI thread; lock-free.
	uint64_t audio_age_ns() const { return last_window_age_ns_.load(); }
	// True once the raw audio tap has been connected and has seen at least
	// one sample in the current process lifetime.
	bool audio_tap_active() const { return audio_tap_active_.load(); }

	// Task 15 review ruling 1: libobs sends a NULL encoder_packet from
	// full_stop() on an encoder error. The encoded_packet trampoline calls
	// this instead of on_video_packet so audio stops flowing into the queue
	// too, without video.
	void stop_accepting() { accepting_ = false; }

	// Task 15 review ruling 3: lets the output's create/destroy callbacks
	// track (and, from stop(), clear) the per-instance data's owner pointer
	// before the output is released, closing the window where the
	// encoded_packet trampoline could still see this object mid-teardown.
	void attach_output_data(void *data) { output_data_ = data; }

private:
	static void audio_trampoline(void *param, size_t mix_idx, struct audio_data *data);

	airplay::MediaQueue &queue_;
	obs_output_t *output_ = nullptr;
	obs_encoder_t *encoder_ = nullptr;
	std::string encoder_id_;
	airplay::KeyframeGate keyframe_gate_{airplay::kKeyframeGateNs};
	QTimer keyframe_timer_; // UI thread: the one deferred request_keyframe()
	size_t mix_idx_ = 0;
	bool audio_connected_ = false;
	std::atomic<bool> accepting_{false};
	// Opaque OutputData*, owned by libobs; see attach_output_data() above.
	void *output_data_ = nullptr;
	// Encoder thread only (reset on the UI thread before the output starts).
	std::vector<uint8_t> headers_;
	bool headers_checked_ = false;

	// Ruling R7: audio-age tracking. Written only from on_audio() (the audio
	// thread); read from any thread. current_window_age_ns_ accumulates the
	// max age seen in the current ~2 s window; when the window elapses,
	// its value is published to last_window_age_ns_ and the window resets.
	// Never blocks: plain atomics, no locks, no allocation.
	std::atomic<uint64_t> current_window_age_ns_{0};
	std::atomic<uint64_t> last_window_age_ns_{0};
	std::atomic<uint64_t> window_started_ns_{0};
	std::atomic<bool> audio_tap_active_{false};
};
