// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/events.hpp"

#include <nlohmann/json.hpp>

#include <limits>

namespace airplay {
namespace {

using nlohmann::json;

std::string str_field(const json &j, const char *key)
{
	const auto it = j.find(key);
	return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

int int_field(const json &j, const char *key)
{
	const auto it = j.find(key);
	if (it == j.end() || !it->is_number_integer())
		return 0;
	const int64_t v = it->get<int64_t>();
	if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max())
		return 0;
	return static_cast<int>(v);
}

} // namespace

DisplayState parse_display_state(const std::string &s)
{
	if (s == "idle")
		return DisplayState::Idle;
	if (s == "offline")
		return DisplayState::Offline;
	if (s == "connecting")
		return DisplayState::Connecting;
	if (s == "credential")
		return DisplayState::Credential;
	if (s == "live")
		return DisplayState::Live;
	if (s == "retrying")
		return DisplayState::Retrying;
	if (s == "failed")
		return DisplayState::Failed;
	return DisplayState::Unknown;
}

const char *to_string(DisplayState s)
{
	switch (s) {
	case DisplayState::Idle:
		return "idle";
	case DisplayState::Offline:
		return "offline";
	case DisplayState::Connecting:
		return "connecting";
	case DisplayState::Credential:
		return "credential";
	case DisplayState::Live:
		return "live";
	case DisplayState::Retrying:
		return "retrying";
	case DisplayState::Failed:
		return "failed";
	case DisplayState::Unknown:
		break;
	}
	return "unknown";
}

Event parse_event(const std::string &line)
{
	const json j = json::parse(line, nullptr, false);
	if (j.is_discarded() || !j.is_object())
		return UnknownEvent{line};

	const std::string name = str_field(j, "event");
	if (name == "ready") {
		ReadyEvent ev{int_field(j, "version"), {}};
		const auto it = j.find("capabilities");
		if (it != j.end() && it->is_array())
			for (const json &c : *it)
				if (c.is_string())
					ev.capabilities.push_back(c.get<std::string>());
		return ev;
	}

	if (name == "devices") {
		DevicesEvent ev;
		const auto it = j.find("devices");
		if (it != j.end() && it->is_array()) {
			for (const json &d : *it) {
				if (!d.is_object())
					continue;
				Device dev{str_field(d, "device_id"), str_field(d, "name"), str_field(d, "model"),
					   str_field(d, "ip"), int_field(d, "port")};
				if (!dev.device_id.empty())
					ev.devices.push_back(std::move(dev));
			}
		}
		return ev;
	}

	// "display" is the canonical name; "room" is what helpers before the rename send.
	if (name == "display" || name == "room") {
		DisplayEvent ev;
		ev.device_id = str_field(j, "device_id");
		if (ev.device_id.empty())
			return UnknownEvent{line};
		ev.state_text = str_field(j, "state");
		ev.state = parse_display_state(ev.state_text);
		ev.credential_kind = str_field(j, "credential_kind");
		ev.error = str_field(j, "error");
		ev.audio = str_field(j, "audio");
		ev.audio_reason = str_field(j, "audio_reason");
		return ev;
	}

	if (name == "keyframe") {
		KeyframeEvent ev{str_field(j, "device_id"), str_field(j, "reason")};
		if (ev.device_id.empty())
			return UnknownEvent{line};
		return ev;
	}

	if (name == "delivery") {
		DeliveryEvent ev;
		ev.device_id = str_field(j, "device_id");
		if (ev.device_id.empty())
			return UnknownEvent{line};
		ev.frames = int_field(j, "frames");
		ev.p50_ms = int_field(j, "p50_ms");
		ev.p99_ms = int_field(j, "p99_ms");
		ev.late = int_field(j, "late");
		ev.lead_ms = int_field(j, "lead_ms");
		ev.audio_lost = int_field(j, "audio_lost");
		ev.audio_resent = int_field(j, "audio_resent");
		ev.audio_dropped = int_field(j, "audio_dropped");
		ev.audio_jumps = int_field(j, "audio_jumps");
		ev.lead_target_ms = int_field(j, "lead_target_ms");
		ev.lead_ceiling_ms = int_field(j, "lead_ceiling_ms");
		return ev;
	}

	if (name == "fatal")
		return FatalEvent{str_field(j, "error")};

	return UnknownEvent{line};
}

LineSplitter::LineSplitter(size_t max_line) : max_line_(max_line) {}

void LineSplitter::feed(const char *data, size_t size, const std::function<void(const std::string &)> &on_line)
{
	for (size_t i = 0; i < size; ++i) {
		const char c = data[i];
		if (c == '\n') {
			if (!discarding_) {
				if (!buf_.empty() && buf_.back() == '\r')
					buf_.pop_back();
				on_line(buf_);
			}
			buf_.clear();
			discarding_ = false;
			continue;
		}
		if (discarding_)
			continue;
		if (buf_.size() >= max_line_) {
			buf_.clear();
			discarding_ = true;
			continue;
		}
		buf_.push_back(c);
	}
}

} // namespace airplay
