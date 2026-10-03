// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Per-display playout lead (spec 2026-09-28 section 6): the time from OBS
// capture to presentation on that TV, the same for its video and audio so
// the TV stays in sync with itself. TVs may run ahead of OBS and of each other.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace airplay {

struct LeadInputs {
	int fixed_ms = 0;        // this display's explicit lead; 0 = Auto
	int global_fixed_ms = 0; // legacy global target latency; 0 = none
	bool audio_on = true;
	bool wifi = false;
	uint64_t audio_age_ns = 0; // OBS audio age at the plugin tap; 0 = unknown
	int video_need_ms = 0;     // learned capture-to-write p99; 0 = unknown
	int late_floor_ms = 0;     // lead reached by a late-frame raise; Auto never goes below it
	int saved_lead_ms = 0;     // Auto's saved starting lead (live_lead helpers); 0 = use the formula
};

constexpr int kLeadFloorMs = 40;
constexpr int kLeadCeilingMs = 2000;
constexpr int kDefaultVideoNeedMs = 30;
constexpr int kEldFrameMs = 11;
constexpr int kWiredMarginMs = 20;
constexpr int kWifiMarginMs = 70;
// Helper reframing, the AAC-ELD encoder round trip and RTP send, added to the
// audio need on top of the OBS audio age and one ELD frame. To be measured in P5.
constexpr int kAudioPathMarginMs = 50;
// A session lead restarts only for a rise of at least this much, so small
// swings in the measured audio age do not restart a TV.
constexpr int kLeadRestartStepMs = 20;
// Spec section 6: repeated late frames raise a display's lead by this much.
constexpr int kLateRaiseMs = 20;
// Legacy GlobalSettings::auto_latency_ms was the OBS audio age plus this.
constexpr int kLegacyAutoLatencyMarginMs = 150;

int display_lead_ms(const LeadInputs &in);
// The stored video need raised to an observed p99, the p99 first capped at
// cap_ms when that is > 0 (the display's lead: a need above it shows up as
// late frames, which the late path handles). The result stays in [0, 2000].
int raise_video_need(int stored_ms, int observed_p99_ms, int cap_ms = 0);
int learn_video_need_at_start(int stored_ms, int last_run_p99_ms);
bool lead_needs_restart(int applied_ms, int wanted_ms);
// The late floor after a late-frame raise from a session running applied_ms.
int late_floor_after_raise(int applied_ms);
// At start: the late floor is cleared only when the last run had no late windows.
int learn_late_floor_at_start(int floor_ms, int last_run_late_windows);
// First run after install: the last audio age from the legacy auto latency, so
// audio TVs start with about the right lead instead of restarting once measured.
int seed_audio_age_ms(int last_audio_age_ms, int legacy_auto_latency_ms);

// One display's delivery windows with late frames (spec section 6: more than
// 3 in 60 s raise its lead) and its late-driven restarts (at most one per 10
// minutes).
class LateWindows {
public:
	static constexpr uint64_t kWindowNs = 60ull * 1000 * 1000 * 1000;
	static constexpr int kWindowsToRaise = 3;
	static constexpr uint64_t kRestartIntervalNs = 10ull * 60 * 1000 * 1000 * 1000;

	// Records one delivery window. True when it makes more than
	// kWindowsToRaise late windows within kWindowNs; the history then clears.
	bool record(uint64_t now_ns, int late_frames);
	// Late windows recorded this run.
	int run_late_windows() const { return run_late_windows_; }
	// True, and noted, when no late-driven restart happened in the last 10 minutes.
	bool take_restart(uint64_t now_ns);

private:
	std::vector<uint64_t> recent_ns_;
	int run_late_windows_ = 0;
	uint64_t last_restart_ns_ = 0;
	bool restarted_ = false;
};

// Audio packets one display's receiver asked to be resent (lost on the network),
// from delivery events. Shown beside the TV delay so an operator can lower the
// delay while losses stay at zero, and raise it when they start: a resend has
// to arrive within the delay to be heard.
class AudioLoss {
public:
	static constexpr uint64_t kWindowNs = 60ull * 1000 * 1000 * 1000;

	struct Counts {
		int lost = 0;       // packets the receiver asked for again
		int not_resent = 0; // of those, already gone from the sender's history
		int dropped = 0;    // frames the sender dropped as too late for the lead
	};

	void record(uint64_t now_ns, int lost, int resent, int dropped = 0);
	// Counts over the kWindowNs before now_ns.
	Counts last_minute(uint64_t now_ns) const;
	// Counts since the plugin started.
	Counts run() const { return run_; }
	// True once any delivery event has been recorded.
	bool seen() const { return seen_; }

private:
	struct Sample {
		uint64_t ns;
		Counts counts;
	};
	std::vector<Sample> recent_;
	Counts run_;
	bool seen_ = false;
};

// Status text for one display, e.g. "audio lost 5, 3 dropped late in the last
// minute (2 not resent), 12 this run" or "no audio loss in the last minute".
// Counts toward an Auto TV delay raise: a delivery window with late video, audio
// dropped as late, or lost audio the sender could not resend. Losses that were
// resent are shown but not counted: Wi-Fi loses a packet now and then at any
// delay, and counting those would keep Auto from ever lowering the delay.
int lead_trouble(int late_frames, int audio_lost, int audio_resent, int audio_dropped);

std::string audio_loss_text(const AudioLoss &loss, uint64_t now_ns);

} // namespace airplay
