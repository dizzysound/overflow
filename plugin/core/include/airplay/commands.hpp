// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// JSON payloads for command (0x10) frames. See docs/protocol.md and revision 1.1.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace airplay {

struct DisplaySelection {
	std::string device_id;
	bool auto_reconnect = true;
	bool audio = true; // false: video-only session; omitted from the wire when true
	bool wifi_tolerant = false; // true: 250 ms relay budget; omitted from the wire when false
	int latency_ms = 0;         // playout lead; omitted from the wire when 0
	int lead_headroom_ms = 0;   // revision 1.4: Auto TV delay may slide this much above latency_ms; omitted when 0
	std::string audio_format;   // "alac" or "aac-eld"; omitted from the wire when empty (Auto)
	std::optional<double> volume_db; // unset: the helper never changes volume
	std::string ip;                  // manual address; empty = discovered
	int port = 0;                    // with ip only; 0 = the helper's 7000
};

bool operator==(const DisplaySelection &a, const DisplaySelection &b);
bool operator!=(const DisplaySelection &a, const DisplaySelection &b);

std::string set_displays_command(const std::vector<DisplaySelection> &displays);
std::string set_lead_command(const std::string &device_id, int lead_ms);
std::string credential_command(const std::string &device_id, const std::string &value);
std::string reconnect_command(const std::string &device_id);
std::string restart_command(const std::string &device_id);
std::string forget_command(const std::string &device_id);
std::string set_volume_command(const std::string &device_id, double volume_db);
std::string shutdown_command();

} // namespace airplay
