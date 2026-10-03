// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/commands.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

using airplay::DisplaySelection;

TEST_CASE("set_displays always sends auto_reconnect and omits unset fields")
{
	DisplaySelection a;
	a.device_id = "AA:BB";
	a.volume_db = -12.0;
	DisplaySelection b;
	b.device_id = "0C:FB:30:58:DF:2E";
	b.auto_reconnect = false;
	b.ip = "10.20.0.178";
	b.port = 7000;
	const std::string expected = "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"AA:BB\",\"auto_reconnect\":true,\"volume_db\":-12.0},"
		"{\"device_id\":\"0C:FB:30:58:DF:2E\",\"auto_reconnect\":false,\"ip\":\"10.20.0.178\",\"port\":7000}]}";
	CHECK(airplay::set_displays_command({a, b}) == expected);
}

TEST_CASE("set_displays sends audio:false only when audio is off, and omits it when on")
{
	DisplaySelection off;
	off.device_id = "A";
	off.audio = false;
	CHECK(airplay::set_displays_command({off}) ==
	      "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"A\",\"auto_reconnect\":true,\"audio\":false}]}");

	DisplaySelection on;
	on.device_id = "B";
	CHECK(airplay::set_displays_command({on}) ==
	      "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"B\",\"auto_reconnect\":true}]}");
}

TEST_CASE("set_displays sends wifi_tolerant:true only when set, and omits it otherwise")
{
	DisplaySelection wifi;
	wifi.device_id = "A";
	wifi.wifi_tolerant = true;
	CHECK(airplay::set_displays_command({wifi}) ==
	      "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"A\",\"auto_reconnect\":true,\"wifi_tolerant\":true}]}");

	DisplaySelection wired;
	wired.device_id = "B";
	CHECK(airplay::set_displays_command({wired}) ==
	      "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"B\",\"auto_reconnect\":true}]}");
}

TEST_CASE("DisplaySelection equality includes wifi_tolerant")
{
	DisplaySelection a;
	a.device_id = "A";
	DisplaySelection b = a;
	CHECK(a == b);
	b.wifi_tolerant = true;
	CHECK(a != b);
}

TEST_CASE("DisplaySelection equality includes audio")
{
	DisplaySelection a;
	a.device_id = "A";
	DisplaySelection b = a;
	CHECK(a == b);
	b.audio = false;
	CHECK(a != b);
}

TEST_CASE("set_displays with no displays deselects everything")
{
	CHECK(airplay::set_displays_command({}) == "{\"cmd\":\"set_displays\",\"displays\":[]}");
}

TEST_CASE("set_displays never sends a port without an ip")
{
	DisplaySelection r;
	r.device_id = "X";
	r.port = 7000;
	CHECK(airplay::set_displays_command({r}) == "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"X\",\"auto_reconnect\":true}]}");
}

TEST_CASE("per-device commands")
{
	CHECK(airplay::credential_command("A", "1234") == "{\"cmd\":\"credential\",\"device_id\":\"A\",\"value\":\"1234\"}");
	CHECK(airplay::reconnect_command("A") == "{\"cmd\":\"reconnect\",\"device_id\":\"A\"}");
	CHECK(airplay::restart_command("A") == "{\"cmd\":\"restart\",\"device_id\":\"A\"}");
	CHECK(airplay::forget_command("A") == "{\"cmd\":\"forget\",\"device_id\":\"A\"}");
	CHECK(airplay::set_volume_command("A", -15.5) == "{\"cmd\":\"set_volume\",\"device_id\":\"A\",\"volume_db\":-15.5}");
	CHECK(airplay::shutdown_command() == "{\"cmd\":\"shutdown\"}");
}

TEST_CASE("credential values are JSON-escaped")
{
	// A quote and a backslash in the value survive a JSON round trip exactly,
	// and the escaped form is on the wire.
	const std::string value = "pa\"s\\s";
	const std::string result = airplay::credential_command("A", value);
	CHECK(result == "{\"cmd\":\"credential\",\"device_id\":\"A\",\"value\":\"pa\\\"s\\\\s\"}");
	const auto parsed = nlohmann::json::parse(result);
	CHECK(parsed.at("value").get<std::string>() == value);
}

TEST_CASE("DisplaySelection equality covers every field")
{
	DisplaySelection a;
	a.device_id = "A";
	DisplaySelection b = a;
	CHECK(a == b);
	b.volume_db = -3.0;
	CHECK(a != b);
	b = a;
	b.ip = "10.0.0.1";
	CHECK(a != b);
}

TEST_CASE("set_displays sends latency_ms only when set, after wifi_tolerant")
{
	DisplaySelection a;
	a.device_id = "A";
	a.wifi_tolerant = true;
	a.latency_ms = 95;
	CHECK(airplay::set_displays_command({a}) ==
	      "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"A\",\"auto_reconnect\":true,\"wifi_tolerant\":true,\"latency_ms\":95}]}");

	DisplaySelection b;
	b.device_id = "B";
	CHECK(airplay::set_displays_command({b}) ==
	      "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"B\",\"auto_reconnect\":true}]}");
}

TEST_CASE("set_displays sends audio_format only when forced")
{
	DisplaySelection forced;
	forced.device_id = "A";
	forced.audio_format = "aac-eld";
	CHECK(airplay::set_displays_command({forced}) ==
	      "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"A\",\"auto_reconnect\":true,\"audio_format\":\"aac-eld\"}]}");
	DisplaySelection automatic = forced;
	automatic.audio_format.clear();
	CHECK(forced != automatic);
	CHECK(airplay::set_displays_command({automatic}) ==
	      "{\"cmd\":\"set_displays\",\"displays\":[{\"device_id\":\"A\",\"auto_reconnect\":true}]}");
}

TEST_CASE("set_displays: lead_headroom_ms only when positive; set_lead command")
{
	airplay::DisplaySelection a;
	a.device_id = "AA";
	a.latency_ms = 70;
	a.lead_headroom_ms = 60;
	airplay::DisplaySelection b = a;
	b.lead_headroom_ms = 0;
	const std::string with = airplay::set_displays_command({a});
	const std::string without = airplay::set_displays_command({b});
	CHECK(with.find(R"("lead_headroom_ms":60)") != std::string::npos);
	CHECK(without.find("lead_headroom_ms") == std::string::npos);
	CHECK(a != b);
	CHECK(airplay::set_lead_command("AA", 75) == R"({"cmd":"set_lead","device_id":"AA","lead_ms":75})");
}
