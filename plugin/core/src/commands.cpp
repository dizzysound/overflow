// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/commands.hpp"

#include <nlohmann/json.hpp>

namespace airplay {
namespace {

using Json = nlohmann::ordered_json;

std::string dump(const Json &j)
{
	// Replace invalid UTF-8 rather than throw.
	return j.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::string device_command(const char *cmd, const std::string &device_id)
{
	Json j;
	j["cmd"] = cmd;
	j["device_id"] = device_id;
	return dump(j);
}

} // namespace

bool operator==(const DisplaySelection &a, const DisplaySelection &b)
{
	return a.device_id == b.device_id && a.auto_reconnect == b.auto_reconnect && a.audio == b.audio &&
	       a.wifi_tolerant == b.wifi_tolerant && a.latency_ms == b.latency_ms && a.lead_headroom_ms == b.lead_headroom_ms && a.audio_format == b.audio_format && a.volume_db == b.volume_db && a.ip == b.ip && a.port == b.port;
}

bool operator!=(const DisplaySelection &a, const DisplaySelection &b)
{
	return !(a == b);
}

std::string set_displays_command(const std::vector<DisplaySelection> &displays)
{
	Json list = Json::array();
	for (const DisplaySelection &r : displays) {
		Json e;
		e["device_id"] = r.device_id;
		e["auto_reconnect"] = r.auto_reconnect;
		if (!r.audio)
			e["audio"] = false;
		if (r.wifi_tolerant)
			e["wifi_tolerant"] = true;
		if (r.latency_ms > 0)
			e["latency_ms"] = r.latency_ms;
		if (r.lead_headroom_ms > 0)
			e["lead_headroom_ms"] = r.lead_headroom_ms;
		if (!r.audio_format.empty())
			e["audio_format"] = r.audio_format;
		if (r.volume_db)
			e["volume_db"] = *r.volume_db;
		if (!r.ip.empty()) {
			e["ip"] = r.ip;
			if (r.port > 0)
				e["port"] = r.port;
		}
		list.push_back(std::move(e));
	}
	Json j;
	j["cmd"] = "set_displays";
	j["displays"] = std::move(list);
	return dump(j);
}

std::string set_lead_command(const std::string &device_id, int lead_ms)
{
	Json j;
	j["cmd"] = "set_lead";
	j["device_id"] = device_id;
	j["lead_ms"] = lead_ms;
	return dump(j);
}

std::string credential_command(const std::string &device_id, const std::string &value)
{
	Json j;
	j["cmd"] = "credential";
	j["device_id"] = device_id;
	j["value"] = value;
	return dump(j);
}

std::string reconnect_command(const std::string &device_id)
{
	return device_command("reconnect", device_id);
}

std::string restart_command(const std::string &device_id)
{
	return device_command("restart", device_id);
}

std::string forget_command(const std::string &device_id)
{
	return device_command("forget", device_id);
}

std::string set_volume_command(const std::string &device_id, double volume_db)
{
	Json j;
	j["cmd"] = "set_volume";
	j["device_id"] = device_id;
	j["volume_db"] = volume_db;
	return dump(j);
}

std::string shutdown_command()
{
	return R"({"cmd":"shutdown"})";
}

} // namespace airplay
