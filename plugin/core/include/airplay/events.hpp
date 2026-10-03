// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Helper -> plugin stdout events (JSON lines). See docs/protocol.md.
#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace airplay {

struct Device {
	std::string device_id;
	std::string name;
	std::string model;
	std::string ip;
	int port = 0;
};

enum class DisplayState { Idle, Offline, Connecting, Credential, Live, Retrying, Failed, Unknown };

DisplayState parse_display_state(const std::string &s);
const char *to_string(DisplayState s);

struct DisplayEvent {
	std::string device_id;
	DisplayState state = DisplayState::Unknown;
	std::string state_text;      // the state as sent
	std::string credential_kind; // "pin" or "password" in the credential state
	std::string error;
	std::string audio;           // live only (revision 1.1): "on" or "off"
	std::string audio_reason;    // with audio "off"
};

struct ReadyEvent {
	int version = 0;
	std::vector<std::string> capabilities; // revision 1.4: e.g. "live_lead"
	bool has(const std::string &capability) const
	{
		return std::find(capabilities.begin(), capabilities.end(), capability) != capabilities.end();
	}
};

struct DevicesEvent {
	std::vector<Device> devices;
};

struct FatalEvent {
	std::string error;
};

struct KeyframeEvent {
	std::string device_id;
	std::string reason;
};

struct DeliveryEvent {
	std::string device_id;
	int frames = 0;
	int p50_ms = 0;
	int p99_ms = 0;
	int late = 0;
	int lead_ms = 0;
	int audio_lost = 0;   // revision 1.3: audio packets the receiver asked to be resent
	int audio_resent = 0; // revision 1.3: of those, resent from the history
	int audio_dropped = 0; // revision 1.3: audio frames dropped at the sender as too late for the lead
	int audio_jumps = 0;   // revision 1.5: audio RTP timeline discontinuities (0 when healthy)
	int lead_target_ms = 0;  // revision 1.4: the lead the helper is steering toward
	int lead_ceiling_ms = 0; // revision 1.4: the most the lead may slide up to
};

struct UnknownEvent {
	std::string raw;
};

using Event = std::variant<ReadyEvent, DevicesEvent, DisplayEvent, FatalEvent, KeyframeEvent, DeliveryEvent, UnknownEvent>;

// Parses one stdout line. Never throws. Both "display" events and their older
// alias, "room", become DisplayEvent.
Event parse_event(const std::string &line);

// Splits a byte stream into lines. Not thread-safe; use one per stream.
class LineSplitter {
public:
	explicit LineSplitter(size_t max_line = 1 << 20);
	void feed(const char *data, size_t size, const std::function<void(const std::string &)> &on_line);

private:
	std::string buf_;
	size_t max_line_;
	bool discarding_ = false;
};

} // namespace airplay
