// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/settings.hpp"
#include "airplay/settings_store.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace airplay;

namespace {
Settings sample()
{
	Settings s;
	s.global.audio_track = 2;
	s.global.preset = QualityPreset::P720_4Mbps;
	s.global.encoder_override = "obs_x264";
	s.global.start_on_launch = true;
	s.global.start_with_recording = true;
	s.global.target_latency_ms = 250;
	s.global.auto_latency_ms = 850;
	s.global.timing = TimingMode::Ntp;
	s.global.eld_encoder = false;
	s.global.verbose_helper_log = true;
	DisplaySettings &a = s.ensure_display("9E:B8:AE:9A:2A:CF", "Sacristy");
	a.location = "Sacristy";
	a.enabled = true;
	a.audio_enabled = false;
	a.volume_db = -12.5;
	a.idle_policy = IdlePolicy::DisconnectAfterIdle;
	a.idle_minutes = 45;
	a.password_protected = "AQAAANCMnd8=";
	DisplaySettings &b = s.ensure_display("0C:FB:30:58:DF:2E", "Roku");
	b.auto_reconnect = false;
	b.manual_ip = "10.20.0.178";
	b.manual_port = 7000;
	b.wifi_tolerant = true;
	b.idle_policy = IdlePolicy::StayConnected;
	return s;
}

std::filesystem::path temp_dir()
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	auto dir = std::filesystem::temp_directory_path() / ("airplay-settings-" + std::to_string(stamp));
	std::filesystem::create_directories(dir);
	return dir;
}
} // namespace

TEST_CASE("defaults")
{
	const Settings s;
	CHECK(s.global.audio_track == 1);
	CHECK(s.global.preset == QualityPreset::P1080_6Mbps);
	CHECK(s.global.timing == TimingMode::Auto);
	CHECK(s.global.eld_encoder);
	CHECK_FALSE(s.global.start_on_launch);
	CHECK_FALSE(s.global.verbose_helper_log);
	const DisplaySettings r;
	CHECK_FALSE(r.volume_db.has_value());
	CHECK(r.auto_reconnect);
	CHECK(r.audio_enabled);
	CHECK_FALSE(r.wifi_tolerant);
	CHECK(r.idle_policy == IdlePolicy::DisconnectWhenOutputStops);
	CHECK(s.global.auto_latency_ms == 0);
}

TEST_CASE("audio_enabled defaults to true when the field is absent from an older settings file")
{
	const std::string legacy = R"({"version":1,"global":{},"displays":[
		{"device_id":"A","display_name":"Roku","enabled":true}
	]})";
	std::string warning;
	const Settings s = settings_from_json(legacy, &warning);
	CHECK(warning.empty());
	REQUIRE(s.displays.size() == 1);
	CHECK(s.displays[0].audio_enabled);
}

TEST_CASE("wifi_tolerant defaults to false when the field is absent from an older settings file")
{
	const std::string legacy = R"({"version":1,"global":{},"displays":[
		{"device_id":"A","display_name":"Roku","enabled":true}
	]})";
	std::string warning;
	const Settings s = settings_from_json(legacy, &warning);
	CHECK(warning.empty());
	REQUIRE(s.displays.size() == 1);
	CHECK_FALSE(s.displays[0].wifi_tolerant);
}

TEST_CASE("DisplaySettings equality includes wifi_tolerant")
{
	DisplaySettings a;
	a.device_id = "A";
	DisplaySettings b = a;
	CHECK(a == b);
	b.wifi_tolerant = true;
	CHECK_FALSE(a == b);
}

TEST_CASE("auto_latency_ms round-trips and clamps to [0, 2000]")
{
	Settings s;
	s.global.auto_latency_ms = 900;
	std::string warning;
	const Settings back = settings_from_json(settings_to_json(s), &warning);
	CHECK(back.global.auto_latency_ms == 900);

	const std::string out_of_range = R"({"version":1,"global":{"auto_latency_ms":5000},"displays":[]})";
	const Settings clamped = settings_from_json(out_of_range, &warning);
	CHECK(clamped.global.auto_latency_ms == 0); // out of range falls back to the default, like every other field
}

TEST_CASE("last_audio_age_ms round-trips, clamps to [0, 2000], and counts in operator==")
{
	Settings s;
	CHECK(s.global.last_audio_age_ms == 0);
	s.global.last_audio_age_ms = 64;
	std::string warning;
	const Settings back = settings_from_json(settings_to_json(s), &warning);
	CHECK(back.global.last_audio_age_ms == 64);
	CHECK(back.global == s.global);

	const std::string out_of_range = R"({"version":1,"global":{"last_audio_age_ms":5000},"displays":[]})";
	CHECK(settings_from_json(out_of_range, &warning).global.last_audio_age_ms == 0);

	GlobalSettings a;
	GlobalSettings b;
	b.last_audio_age_ms = 1;
	CHECK_FALSE(a == b);
}

TEST_CASE("JSON round trip")
{
	const Settings s = sample();
	std::string warning;
	const Settings back = settings_from_json(settings_to_json(s), &warning);
	CHECK(warning.empty());
	CHECK(back.global == s.global);
	REQUIRE(back.displays.size() == 2);
	CHECK(back.displays[0] == s.displays[0]);
	CHECK(back.displays[1] == s.displays[1]);
}

TEST_CASE("quality preset strings round-trip for all four values")
{
	struct Case {
		QualityPreset preset;
		std::string text;
	};
	const std::vector<Case> cases = {
		{QualityPreset::P1080_6Mbps, "1080p_6mbps"},
		{QualityPreset::P1080_8Mbps, "1080p_8mbps"},
		{QualityPreset::P1080_10Mbps, "1080p_10mbps"},
		{QualityPreset::P720_4Mbps, "720p_4mbps"},
	};
	for (const Case &c : cases) {
		CHECK(std::string(to_string(c.preset)) == c.text);

		QualityPreset parsed = QualityPreset::P1080_8Mbps;
		REQUIRE(parse_quality_preset(c.text, &parsed));
		CHECK(parsed == c.preset);

		Settings s;
		s.global.preset = c.preset;
		std::string warning;
		const Settings back = settings_from_json(settings_to_json(s), &warning);
		CHECK(warning.empty());
		CHECK(back.global.preset == c.preset);
	}
}

TEST_CASE("unknown quality preset strings fall back to the default silently")
{
	std::string warning;
	const Settings old_name = settings_from_json(R"({"global":{"preset":"720p_5mbps"}})", &warning);
	CHECK(warning.empty());
	CHECK(old_name.global.preset == QualityPreset::P1080_6Mbps);

	std::string warning2;
	const Settings bogus = settings_from_json(R"({"global":{"preset":"4k"}})", &warning2);
	CHECK(warning2.empty());
	CHECK(bogus.global.preset == QualityPreset::P1080_6Mbps);
}

TEST_CASE("location is optional and round-trips")
{
	Settings s;
	s.ensure_display("A", "Left TV").location = "Friendship Hall";
	s.ensure_display("B", "Lobby TV");
	std::string warning;
	const Settings back = settings_from_json(settings_to_json(s), &warning);
	CHECK(back.displays[0].location == "Friendship Hall");
	CHECK(back.displays[1].location.empty());
	const Settings old = settings_from_json(R"({"displays":[{"device_id":"C"}]})", &warning);
	CHECK(old.displays[0].location.empty());
}

TEST_CASE("unset volume is written as null and read back as unset")
{
	Settings s;
	s.ensure_display("A", "A");
	const std::string text = settings_to_json(s);
	CHECK(text.find("\"volume_db\": null") != std::string::npos);
	std::string warning;
	CHECK_FALSE(settings_from_json(text, &warning).displays[0].volume_db.has_value());
}

TEST_CASE("bad values fall back to defaults field by field")
{
	const std::string text = R"({"global":{"audio_track":9,"preset":"4k","timing":"ntp","target_latency_ms":-5},
		"displays":[{"device_id":""},{"device_id":"A","volume_db":-40,"idle_policy":"after_idle","idle_minutes":15,"manual_port":70000},
		{"device_id":"A","display_name":"duplicate"},"junk"]})";
	std::string warning;
	const Settings s = settings_from_json(text, &warning);
	CHECK(warning.empty());
	CHECK(s.global.audio_track == 1);
	CHECK(s.global.preset == QualityPreset::P1080_6Mbps);
	CHECK(s.global.timing == TimingMode::Ntp);
	CHECK(s.global.target_latency_ms == 0);
	REQUIRE(s.displays.size() == 1);
	CHECK(s.displays[0].device_id == "A");
	CHECK_FALSE(s.displays[0].volume_db.has_value());
	CHECK(s.displays[0].idle_policy == IdlePolicy::DisconnectAfterIdle);
	CHECK(s.displays[0].idle_minutes == 15);
	CHECK(s.displays[0].manual_port == 0);
}

TEST_CASE("invalid JSON gives defaults and a warning")
{
	std::string warning;
	const Settings s = settings_from_json("{not json", &warning);
	CHECK_FALSE(warning.empty());
	CHECK(s.displays.empty());
	CHECK(s.global.audio_track == 1);
}

TEST_CASE("verbose_helper_log defaults false for an older settings file and round-trips when set")
{
	std::string warning;
	const Settings old = settings_from_json(R"({"global":{"audio_track":2}})", &warning);
	CHECK(warning.empty());
	CHECK_FALSE(old.global.verbose_helper_log);

	Settings s;
	s.global.verbose_helper_log = true;
	const Settings back = settings_from_json(settings_to_json(s), &warning);
	CHECK(warning.empty());
	CHECK(back.global.verbose_helper_log);
}

TEST_CASE("find, ensure and remove displays")
{
	Settings s;
	CHECK(s.find_display("A") == nullptr);
	DisplaySettings &a = s.ensure_display("A", "Narthex");
	CHECK(a.display_name == "Narthex");
	s.ensure_display("A", "ignored").enabled = true;
	REQUIRE(s.displays.size() == 1);
	CHECK(s.displays[0].display_name == "Narthex");
	CHECK(s.displays[0].enabled);
	CHECK(s.remove_display("A"));
	CHECK_FALSE(s.remove_display("A"));
}

TEST_CASE("device ids are uppercased on load")
{
	std::string warning;
	const Settings s = settings_from_json(R"({"displays":[{"device_id":"4a:8a"}]})", &warning);
	CHECK(warning.empty());
	REQUIRE(s.displays.size() == 1);
	const std::string expected = "4A:8A";
	CHECK(s.displays[0].device_id == expected);
}

TEST_CASE("ensure_display and find_display uppercase device ids")
{
	Settings s;
	s.ensure_display("ab", "Test Display");
	const DisplaySettings *found = s.find_display("AB");
	REQUIRE(found != nullptr);
	const std::string expected = "AB";
	CHECK(found->device_id == expected);
}

TEST_CASE("lowercase and uppercase duplicate device ids collapse to one entry")
{
	Settings s;
	s.ensure_display("a", "First");
	s.ensure_display("A", "Second");
	REQUIRE(s.displays.size() == 1);
	CHECK(s.displays[0].display_name == "First");
	CHECK(s.remove_display("A"));
	CHECK(s.displays.empty());

	std::string warning;
	const std::string text =
		R"({"displays":[{"device_id":"a","display_name":"First"},{"device_id":"A","display_name":"Second"}]})";
	const Settings loaded = settings_from_json(text, &warning);
	CHECK(warning.empty());
	REQUIRE(loaded.displays.size() == 1);
	CHECK(loaded.displays[0].display_name == "First");
}

TEST_CASE("file store: missing file, save, load")
{
	const auto dir = temp_dir();
	const std::string path = (dir / "settings.json").string();
	std::string warning;
	CHECK(load_settings_file(path, &warning).displays.empty());
	CHECK(warning.empty());

	std::string error;
	REQUIRE(save_settings_file(path, sample(), &error));
	CHECK(error.empty());
	CHECK_FALSE(std::filesystem::exists(path + ".tmp"));
	const Settings back = load_settings_file(path, &warning);
	CHECK(warning.empty());
	CHECK(back.global == sample().global);
	CHECK(back.displays.size() == 2);

	REQUIRE(save_settings_file(path, Settings{}, &error)); // replaces the existing file
	CHECK(load_settings_file(path, &warning).displays.empty());
	std::filesystem::remove_all(dir);
}

TEST_CASE("Minor 5: an unparsable settings.json is moved aside as .bad, not silently dropped")
{
	const auto dir = temp_dir();
	std::filesystem::create_directories(dir);
	const std::string path = (dir / "settings.json").string();
	const std::string bad_path = path + ".bad";

	{
		std::ofstream out(path, std::ios::binary);
		out << "{ this is not json";
	}
	std::string warning;
	const Settings loaded = load_settings_file(path, &warning);
	CHECK(loaded.displays.empty());
	CHECK(warning.find("not valid JSON") != std::string::npos);
	CHECK_FALSE(std::filesystem::exists(path)); // moved aside, not left in place
	REQUIRE(std::filesystem::exists(bad_path));
	std::string moved_text;
	{
		// Scoped and closed before the next rename: an open handle on
		// bad_path would make the next load_settings_file()'s rename fail on
		// Windows, which does not allow renaming or deleting an open file.
		std::ifstream moved(bad_path, std::ios::binary);
		moved_text.assign((std::istreambuf_iterator<char>(moved)), std::istreambuf_iterator<char>());
	}
	CHECK(moved_text == "{ this is not json");

	// A second bad file replaces the first .bad rather than failing.
	{
		std::ofstream out(path, std::ios::binary);
		out << "[]"; // valid JSON, but not an object: also treated as invalid
	}
	warning.clear();
	CHECK(load_settings_file(path, &warning).displays.empty());
	CHECK(warning.find("not valid JSON") != std::string::npos);
	REQUIRE(std::filesystem::exists(bad_path));
	std::string moved2_text;
	{
		std::ifstream moved2(bad_path, std::ios::binary);
		moved2_text.assign((std::istreambuf_iterator<char>(moved2)), std::istreambuf_iterator<char>());
	}
	CHECK(moved2_text == "[]");

	std::filesystem::remove_all(dir);
}

TEST_CASE("display latency fields round-trip and default to 0")
{
	Settings s;
	DisplaySettings &d = s.ensure_display("AA", "Sacristy");
	d.latency_ms = 120;
	d.video_need_ms = 33;
	d.last_run_p99_ms = 29;
	std::string warning;
	const Settings back = settings_from_json(settings_to_json(s), &warning);
	REQUIRE(back.find_display("AA") != nullptr);
	CHECK(back.find_display("AA")->latency_ms == 120);
	CHECK(back.find_display("AA")->video_need_ms == 33);
	CHECK(back.find_display("AA")->last_run_p99_ms == 29);
	const Settings old = settings_from_json(R"({"displays":[{"device_id":"BB"}]})", &warning);
	CHECK(old.find_display("BB")->latency_ms == 0);
	CHECK(old.find_display("BB")->video_need_ms == 0);
}

TEST_CASE("late_floor_ms and last_run_late_windows round-trip, default to 0, clamp, and count in operator==")
{
	Settings s;
	DisplaySettings &d = s.ensure_display("AA", "Roku");
	CHECK(d.late_floor_ms == 0);
	CHECK(d.last_run_late_windows == 0);
	d.late_floor_ms = 140;
	d.last_run_late_windows = 3;
	std::string warning;
	const Settings back = settings_from_json(settings_to_json(s), &warning);
	REQUIRE(back.find_display("AA") != nullptr);
	CHECK(back.find_display("AA")->late_floor_ms == 140);
	CHECK(back.find_display("AA")->last_run_late_windows == 3);
	CHECK(back.find_display("AA")->saved_lead_ms == 140); // pre-1.4 migration: late floor becomes the saved lead
	d.saved_lead_ms = 140;
	CHECK(*back.find_display("AA") == d);

	const Settings old = settings_from_json(R"({"displays":[{"device_id":"BB"}]})", &warning);
	CHECK(old.find_display("BB")->late_floor_ms == 0);
	CHECK(old.find_display("BB")->last_run_late_windows == 0);
	const Settings bad = settings_from_json(
		R"({"displays":[{"device_id":"CC","late_floor_ms":5000,"last_run_late_windows":-1}]})", &warning);
	CHECK(bad.find_display("CC")->late_floor_ms == 0);
	CHECK(bad.find_display("CC")->last_run_late_windows == 0);

	DisplaySettings a;
	DisplaySettings b = a;
	b.late_floor_ms = 1;
	CHECK_FALSE(a == b);
	b = a;
	b.last_run_late_windows = 1;
	CHECK_FALSE(a == b);
}

TEST_CASE("audio_format round-trips, defaults to Auto, and drops unknown values")
{
	Settings s;
	s.ensure_display("A", "Roku").audio_format = "aac-eld";
	s.ensure_display("B", "Sacristy");
	std::string warning;
	const Settings back = settings_from_json(settings_to_json(s), &warning);
	REQUIRE(back.displays.size() == 2);
	CHECK(back.displays[0].audio_format == "aac-eld");
	CHECK(back.displays[1].audio_format.empty());
	CHECK(back.displays[0] == s.displays[0]);

	const std::string odd = R"({"version":1,"global":{},"displays":[
		{"device_id":"A","display_name":"Roku","audio_format":"opus"}
	]})";
	const Settings cleared = settings_from_json(odd, &warning);
	REQUIRE(cleared.displays.size() == 1);
	CHECK(cleared.displays[0].audio_format.empty());
}

TEST_CASE("settings: lead mode and saved lead round-trip; late floor migrates to saved lead")
{
	std::string warning;
	airplay::Settings s;
	airplay::DisplaySettings d;
	d.device_id = "AA";
	d.lead_mode = airplay::kLeadModeDynamic;
	d.saved_lead_ms = 75;
	s.displays.push_back(d);
	airplay::Settings back = airplay::settings_from_json(airplay::settings_to_json(s), &warning);
	REQUIRE(back.displays.size() == 1);
	CHECK(back.displays[0].lead_mode == "dynamic");
	CHECK(back.displays[0].saved_lead_ms == 75);
	CHECK(back.displays[0] == s.displays[0]);

	// A file from before 1.4: late_floor_ms 120, no saved_lead_ms.
	airplay::Settings old = airplay::settings_from_json(R"({"version":1,"displays":[{"device_id":"AA","late_floor_ms":120}]})", &warning);
	CHECK(old.displays[0].saved_lead_ms == 120);
	CHECK(old.displays[0].lead_mode.empty());
	// Unknown mode values read as raise only.
	airplay::Settings odd = airplay::settings_from_json(R"({"version":1,"displays":[{"device_id":"AA","lead_mode":"turbo"}]})", &warning);
	CHECK(odd.displays[0].lead_mode.empty());
}

TEST_CASE("skip_scenes round-trips, defaults empty, drops bad entries, and counts in operator==")
{
	Settings s;
	s.ensure_display("A", "Narthex").skip_scenes = {"Narthex Wide", "Baptism"};
	s.ensure_display("B", "Sacristy");
	std::string warning;
	const Settings back = settings_from_json(settings_to_json(s), &warning);
	REQUIRE(back.displays.size() == 2);
	CHECK(back.displays[0].skip_scenes == std::vector<std::string>{"Narthex Wide", "Baptism"});
	CHECK(back.displays[1].skip_scenes.empty());
	CHECK(back.displays[0] == s.displays[0]);

	DisplaySettings other = s.displays[0];
	other.skip_scenes.pop_back();
	CHECK(other != s.displays[0]);

	const std::string odd = R"({"version":1,"global":{},"displays":[
		{"device_id":"A","skip_scenes":["Wide", 3, "", "Wide", "Pulpit"]},
		{"device_id":"B","skip_scenes":"Wide"}
	]})";
	const Settings cleaned = settings_from_json(odd, &warning);
	REQUIRE(cleaned.displays.size() == 2);
	CHECK(cleaned.displays[0].skip_scenes == std::vector<std::string>{"Wide", "Pulpit"});
	CHECK(cleaned.displays[1].skip_scenes.empty());
}

TEST_CASE("merge_dialog_fields copies every dialog field and keeps the rest")
{
	DisplaySettings target;
	target.device_id = "AA:BB";
	target.enabled = true;
	target.password_protected = "keep-me";
	target.saved_lead_ms = 210;
	target.video_need_ms = 40;

	DisplaySettings edited;
	edited.device_id = "aa:bb";
	edited.display_name = "Narthex";
	edited.location = "Friendship Hall";
	edited.enabled = false;
	edited.auto_reconnect = false;
	edited.audio_enabled = false;
	edited.wifi_tolerant = true;
	edited.latency_ms = 250;
	edited.lead_mode = kLeadModeDynamic;
	edited.audio_format = "aac-eld";
	edited.volume_db = -6.0;
	edited.manual_ip = "10.20.0.50";
	edited.manual_port = 7100;
	edited.idle_policy = IdlePolicy::StayConnected;
	edited.idle_minutes = 5;
	edited.skip_scenes = {"Narthex Wide"};
	edited.password_protected = "";

	merge_dialog_fields(target, edited);

	CHECK(target.display_name == "Narthex");
	CHECK(target.location == "Friendship Hall");
	CHECK_FALSE(target.auto_reconnect);
	CHECK_FALSE(target.audio_enabled);
	CHECK(target.wifi_tolerant);
	CHECK(target.latency_ms == 250);
	CHECK(target.lead_mode == kLeadModeDynamic);
	CHECK(target.audio_format == "aac-eld");
	CHECK(target.volume_db == -6.0);
	CHECK(target.manual_ip == "10.20.0.50");
	CHECK(target.manual_port == 7100);
	CHECK(target.idle_policy == IdlePolicy::StayConnected);
	CHECK(target.idle_minutes == 5);
	CHECK(target.skip_scenes == std::vector<std::string>{"Narthex Wide"});

	CHECK(target.device_id == "AA:BB");
	CHECK(target.enabled);
	CHECK(target.password_protected == "keep-me");
	CHECK(target.saved_lead_ms == 210);
	CHECK(target.video_need_ms == 40);
}
