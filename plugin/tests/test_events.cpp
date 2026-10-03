// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/events.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace airplay;

TEST_CASE("ready")
{
	const Event e = parse_event(R"({"event":"ready","version":1})");
	REQUIRE(std::holds_alternative<ReadyEvent>(e));
	CHECK(std::get<ReadyEvent>(e).version == 1);
}

TEST_CASE("devices")
{
	const Event e = parse_event(
		R"({"event":"devices","devices":[{"device_id":"9E:B8","name":"Sacristy","model":"AppleTV5,3","ip":"10.20.0.164","port":7000},{"name":"no id"}]})");
	REQUIRE(std::holds_alternative<DevicesEvent>(e));
	const auto &d = std::get<DevicesEvent>(e).devices;
	REQUIRE(d.size() == 1);
	CHECK(d[0].device_id == "9E:B8");
	CHECK(d[0].name == "Sacristy");
	CHECK(d[0].model == "AppleTV5,3");
	CHECK(d[0].ip == "10.20.0.164");
	CHECK(d[0].port == 7000);
}

TEST_CASE("display events, including revision 1.1 audio fields")
{
	Event e = parse_event(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(std::holds_alternative<DisplayEvent>(e));
	CHECK(std::get<DisplayEvent>(e).state == DisplayState::Credential);
	CHECK(std::get<DisplayEvent>(e).credential_kind == "password");

	e = parse_event(
		R"({"event":"display","device_id":"A","state":"live","audio":"off","audio_reason":"receiver only accepts AAC-ELD audio, which this build cannot encode"})");
	const auto &r = std::get<DisplayEvent>(e);
	CHECK(r.state == DisplayState::Live);
	CHECK(r.state_text == "live");
	CHECK(r.audio == "off");
	CHECK(r.audio_reason.find("AAC-ELD") != std::string::npos);

	e = parse_event(R"({"event":"display","device_id":"A","state":"retrying","error":"mirror setup failed: x"})");
	CHECK(std::get<DisplayEvent>(e).state == DisplayState::Retrying);
	CHECK(std::get<DisplayEvent>(e).error == "mirror setup failed: x");
}

TEST_CASE("the older room event name is accepted as an alias")
{
	const Event e = parse_event(R"({"event":"room","device_id":"A","state":"live","audio":"on"})");
	REQUIRE(std::holds_alternative<DisplayEvent>(e));
	CHECK(std::get<DisplayEvent>(e).device_id == "A");
	CHECK(std::get<DisplayEvent>(e).state == DisplayState::Live);
	CHECK(std::get<DisplayEvent>(e).audio == "on");
}

TEST_CASE("display state names")
{
	CHECK(parse_display_state("idle") == DisplayState::Idle);
	CHECK(parse_display_state("offline") == DisplayState::Offline);
	CHECK(parse_display_state("connecting") == DisplayState::Connecting);
	CHECK(parse_display_state("failed") == DisplayState::Failed);
	CHECK(parse_display_state("paused") == DisplayState::Unknown);
	CHECK(std::string(to_string(DisplayState::Retrying)) == "retrying");
}

TEST_CASE("fatal")
{
	const Event e = parse_event(R"({"event":"fatal","error":"bridge: message type 0x02 before hello"})");
	REQUIRE(std::holds_alternative<FatalEvent>(e));
	CHECK(std::get<FatalEvent>(e).error == "bridge: message type 0x02 before hello");
}

TEST_CASE("anything else is Unknown and never throws")
{
	for (const std::string line : {"not json", "[1,2]", R"({"event":"stats"})", R"({"event":"display","state":"live"})",
				       R"({"event":"ready","version":"one"})"}) {
		const Event e = parse_event(line);
		if (line == R"({"event":"ready","version":"one"})") {
			REQUIRE(std::holds_alternative<ReadyEvent>(e));
			CHECK(std::get<ReadyEvent>(e).version == 0);
		} else {
			REQUIRE(std::holds_alternative<UnknownEvent>(e));
			CHECK(std::get<UnknownEvent>(e).raw == line);
		}
	}
}

TEST_CASE("LineSplitter joins chunks and strips CR")
{
	LineSplitter s;
	std::vector<std::string> lines;
	auto collect = [&](const std::string &l) { lines.push_back(l); };
	const std::string a = "{\"a\":1}\r\n{\"b\"";
	const std::string b = ":2}\n\npartial";
	s.feed(a.data(), a.size(), collect);
	s.feed(b.data(), b.size(), collect);
	CHECK(lines == std::vector<std::string>{"{\"a\":1}", "{\"b\":2}", ""});
}

TEST_CASE("LineSplitter drops an overlong line and recovers")
{
	LineSplitter s(8);
	std::vector<std::string> lines;
	const std::string in = "0123456789abcdef\nok\n";
	s.feed(in.data(), in.size(), [&](const std::string &l) { lines.push_back(l); });
	CHECK(lines == std::vector<std::string>{"ok"});
}

TEST_CASE("keyframe event")
{
	const Event e = parse_event(R"({"event":"keyframe","device_id":"AA:BB","reason":"backlog"})");
	REQUIRE(std::holds_alternative<KeyframeEvent>(e));
	CHECK(std::get<KeyframeEvent>(e).device_id == "AA:BB");
	CHECK(std::get<KeyframeEvent>(e).reason == "backlog");
}

TEST_CASE("delivery event")
{
	const Event e = parse_event(
		R"({"event":"delivery","device_id":"AA:BB","frames":300,"p50_ms":18,"p99_ms":41,"late":2,"lead_ms":95})");
	REQUIRE(std::holds_alternative<DeliveryEvent>(e));
	const auto &d = std::get<DeliveryEvent>(e);
	CHECK(d.device_id == "AA:BB");
	CHECK(d.frames == 300);
	CHECK(d.p50_ms == 18);
	CHECK(d.p99_ms == 41);
	CHECK(d.late == 2);
	CHECK(d.lead_ms == 95);
}

TEST_CASE("delivery event without device_id is unknown")
{
	CHECK(std::holds_alternative<UnknownEvent>(parse_event(R"({"event":"delivery","frames":1})")));
}

TEST_CASE("delivery event carries audio loss (revision 1.3), zero when absent")
{
	const Event e = parse_event(
		R"({"event":"delivery","device_id":"AA:BB","frames":300,"p50_ms":18,"p99_ms":41,"late":0,"lead_ms":200,"audio_lost":5,"audio_resent":4,"audio_dropped":7})");
	REQUIRE(std::holds_alternative<DeliveryEvent>(e));
	CHECK(std::get<DeliveryEvent>(e).audio_lost == 5);
	CHECK(std::get<DeliveryEvent>(e).audio_resent == 4);
	CHECK(std::get<DeliveryEvent>(e).audio_dropped == 7);
	const Event old = parse_event(R"({"event":"delivery","device_id":"AA:BB","frames":1,"lead_ms":95})");
	CHECK(std::get<DeliveryEvent>(old).audio_lost == 0);
}

TEST_CASE("ready: capabilities are parsed; an older helper has none")
{
	auto ev = std::get<airplay::ReadyEvent>(airplay::parse_event(R"({"event":"ready","version":1,"capabilities":["live_lead"]})"));
	CHECK(ev.has("live_lead"));
	auto old = std::get<airplay::ReadyEvent>(airplay::parse_event(R"({"event":"ready","version":1})"));
	CHECK_FALSE(old.has("live_lead"));
	auto junk = std::get<airplay::ReadyEvent>(airplay::parse_event(R"({"event":"ready","version":1,"capabilities":"live_lead"})"));
	CHECK_FALSE(junk.has("live_lead"));
}

TEST_CASE("delivery: lead target and ceiling; absent in 1.3 reads 0")
{
	auto dv = std::get<airplay::DeliveryEvent>(airplay::parse_event(
		R"({"event":"delivery","device_id":"AA","frames":300,"p50_ms":20,"p99_ms":40,"late":0,"lead_ms":78,"lead_target_ms":70,"lead_ceiling_ms":135})"));
	CHECK(dv.lead_ms == 78);
	CHECK(dv.lead_target_ms == 70);
	CHECK(dv.lead_ceiling_ms == 135);
	auto old = std::get<airplay::DeliveryEvent>(airplay::parse_event(R"({"event":"delivery","device_id":"AA","lead_ms":78})"));
	CHECK(old.lead_ceiling_ms == 0);
}

TEST_CASE("delivery event carries audio_jumps (revision 1.5), zero when absent")
{
	const Event e = parse_event(
		R"({"event":"delivery","device_id":"AA:BB","frames":300,"lead_ms":200,"audio_jumps":3})");
	REQUIRE(std::holds_alternative<DeliveryEvent>(e));
	CHECK(std::get<DeliveryEvent>(e).audio_jumps == 3);
	const Event old = parse_event(R"({"event":"delivery","device_id":"AA:BB","frames":1,"lead_ms":95})");
	CHECK(std::get<DeliveryEvent>(old).audio_jumps == 0);
}
