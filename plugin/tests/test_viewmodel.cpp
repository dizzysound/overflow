// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/viewmodel.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace airplay;

namespace {

// "Encrypts" by prefixing, so tests can see what was stored.
class FakeProtector final : public SecretProtector {
public:
	bool available() const override { return true; }
	std::optional<std::string> protect(const std::string &plain) const override { return "enc:" + plain; }
	std::optional<std::string> unprotect(const std::string &blob) const override
	{
		if (blob.rfind("enc:", 0) != 0)
			return std::nullopt;
		return blob.substr(4);
	}
	void discard(const std::string &blob) const override { discarded.push_back(blob); }
	mutable std::vector<std::string> discarded;
};

// R6: a non-Windows protector before secure storage is wired up. available()
// is false, and protect()/unprotect() are never expected to be called with it.
class UnavailableProtector final : public SecretProtector {
public:
	bool available() const override { return false; }
	std::optional<std::string> protect(const std::string &) const override { return std::nullopt; }
	std::optional<std::string> unprotect(const std::string &) const override { return std::nullopt; }
};

struct Fixture {
	Settings settings;
	FakeProtector secrets;
	std::vector<std::string> sent;
	std::vector<CredentialPrompt> prompts;
	DisplayListModel vm{settings, secrets, [this](std::string c) { sent.push_back(std::move(c)); },
			    [this](const CredentialPrompt &p) { prompts.push_back(p); }};

	void display(const std::string &json) { vm.apply(parse_event(json)); }
};

// R6 fixture: the protector's available() is false, so any remembered
// password must live only in the model's in-memory map.
struct NoSecretsFixture {
	Settings settings;
	UnavailableProtector secrets;
	std::vector<std::string> sent;
	std::vector<CredentialPrompt> prompts;
	DisplayListModel vm{settings, secrets, [this](std::string c) { sent.push_back(std::move(c)); },
			    [this](const CredentialPrompt &p) { prompts.push_back(p); }};

	void display(const std::string &json) { vm.apply(parse_event(json)); }
};

const DisplayRow *find_row(const std::vector<DisplayRow> &rows, const std::string &id)
{
	for (const DisplayRow &r : rows)
		if (r.device_id == id)
			return &r;
	return nullptr;
}

} // namespace

TEST_CASE("rows merge saved displays with discovered devices, sorted by name")
{
	Fixture f;
	f.settings.ensure_display("B", "studio").enabled = true;
	f.vm.apply(parse_event(
		R"({"event":"devices","devices":[{"device_id":"B","name":"Apple TV","ip":"10.20.0.164"},{"device_id":"C","name":"Lobby","ip":"10.20.0.5"}]})"));
	const auto rows = f.vm.rows();
	REQUIRE(rows.size() == 2);
	CHECK(rows[0].device_id == "C"); // "Lobby" < "studio", case-insensitive
	CHECK_FALSE(rows[0].enabled);
	CHECK(rows[1].display_name == "studio"); // the saved name wins over the discovered one
	CHECK(rows[1].enabled);
	CHECK(rows[1].discovered);
	CHECK(rows[1].ip == "10.20.0.164");
	CHECK(rows[1].light == Light::Gray);
}

TEST_CASE("a saved display that is not discovered still shows")
{
	Fixture f;
	DisplaySettings &r = f.settings.ensure_display("M", "Gallery");
	r.manual_ip = "192.168.1.58";
	const auto rows = f.vm.rows();
	REQUIRE(rows.size() == 1);
	CHECK_FALSE(rows[0].discovered);
	CHECK(rows[0].manual);
	CHECK(rows[0].ip == "192.168.1.58");
}

TEST_CASE("lights, audio badge and the paused-failure hint")
{
	CHECK(light_for_state(DisplayState::Idle) == Light::Gray);
	CHECK(light_for_state(DisplayState::Offline) == Light::Gray);
	CHECK(light_for_state(DisplayState::Connecting) == Light::Yellow);
	CHECK(light_for_state(DisplayState::Credential) == Light::Yellow);
	CHECK(light_for_state(DisplayState::Retrying) == Light::Yellow);
	CHECK(light_for_state(DisplayState::Live) == Light::Green);
	CHECK(light_for_state(DisplayState::Failed) == Light::Red);

	Fixture f;
	f.settings.ensure_display("A", "Gallery").enabled = true;
	f.display(R"({"event":"display","device_id":"A","state":"live","audio":"off","audio_reason":"receiver only accepts AAC-ELD audio"})");
	auto rows = f.vm.rows();
	CHECK(rows[0].light == Light::Green);
	CHECK(rows[0].audio_off);
	CHECK(rows[0].audio_reason == "receiver only accepts AAC-ELD audio");
	CHECK(f.vm.is_live("A"));

	f.display(R"J({"event":"display","device_id":"A","state":"failed","error":"403 at fp-setup (auto-reconnect paused: restart the receiver, then reconnect)"})J");
	rows = f.vm.rows();
	CHECK(rows[0].light == Light::Red);
	CHECK_FALSE(rows[0].audio_off);
	CHECK(rows[0].needs_receiver_restart);
	CHECK(rows[0].status_tooltip.find("Restart the receiver") != std::string::npos);
}

TEST_CASE("Minor 2: a display waiting on a credential hints that Restart re-prompts")
{
	Fixture f;
	f.settings.ensure_display("A", "Studio").enabled = true;
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"pin"})");
	const auto rows = f.vm.rows();
	CHECK(rows[0].status_tooltip.find("Choose Restart to enter the code again.") != std::string::npos);
}

TEST_CASE("a PIN prompt is shown once per episode and the answer is sent")
{
	Fixture f;
	f.settings.ensure_display("A", "Studio").enabled = true;
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"pin"})");
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"pin","error":"x"})");
	REQUIRE(f.prompts.size() == 1);
	CHECK(f.prompts[0].kind == "pin");
	CHECK(f.prompts[0].display_name == "Studio");
	CHECK_FALSE(f.prompts[0].retry);
	f.vm.answer_credential("A", "1234", true);
	REQUIRE(f.sent.size() == 1);
	CHECK(f.sent[0] == R"({"cmd":"credential","device_id":"A","value":"1234"})");
	CHECK(f.settings.find_display("A")->password_protected.empty()); // PINs are never cached
	CHECK_FALSE(f.vm.take_settings_dirty());
}

TEST_CASE("a remembered password answers later prompts automatically")
{
	Fixture f;
	f.settings.ensure_display("A", "Roku").enabled = true;
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(f.prompts.size() == 1);
	f.vm.answer_credential("A", "hunter2", true);
	CHECK(f.settings.find_display("A")->password_protected == "enc:hunter2");
	CHECK(f.vm.take_settings_dirty());

	f.display(R"({"event":"display","device_id":"A","state":"live"})");
	f.display(R"({"event":"display","device_id":"A","state":"retrying","error":"timeout"})");
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	CHECK(f.prompts.size() == 1); // answered from the cache
	REQUIRE(f.sent.size() == 2);
	CHECK(f.sent[1] == R"({"cmd":"credential","device_id":"A","value":"hunter2"})");
}

TEST_CASE("a rejected cached password falls back to asking, marked as a retry")
{
	Fixture f;
	DisplaySettings &r = f.settings.ensure_display("A", "Roku");
	r.enabled = true;
	r.password_protected = "enc:old";
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	CHECK(f.prompts.empty());
	f.display(R"({"event":"display","device_id":"A","state":"retrying","error":"wrong password"})");
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(f.prompts.size() == 1);
	CHECK(f.prompts[0].retry);
}

TEST_CASE("a replaced or forgotten saved password is queued for discard, not discarded")
{
	// The OS-store item may go only after settings.json stops naming it; the
	// controller discards what take_pending_discards() returns after a save.
	Fixture f;
	DisplaySettings &r = f.settings.ensure_display("A", "Roku");
	r.enabled = true;
	r.password_protected = "enc:old";
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	f.display(R"({"event":"display","device_id":"A","state":"retrying","error":"wrong password"})");
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(f.prompts.size() == 1);
	f.vm.answer_credential("A", "new", true);
	CHECK(f.settings.find_display("A")->password_protected == "enc:new");
	CHECK(f.secrets.discarded.empty());
	CHECK(f.vm.take_pending_discards() == std::vector<std::string>{"enc:old"});
	CHECK(f.vm.take_pending_discards().empty());

	f.vm.forget_password("A");
	CHECK(f.settings.find_display("A")->password_protected.empty());
	CHECK(f.vm.take_pending_discards() == std::vector<std::string>{"enc:new"});
	CHECK(f.secrets.discarded.empty());
}

TEST_CASE("cancel, restart, reconnect and forget")
{
	Fixture f;
	DisplaySettings &r = f.settings.ensure_display("A", "Roku");
	r.password_protected = "enc:x";
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"pin"})");
	f.vm.answer_credential("A", "", false);
	CHECK(f.sent.empty());
	f.vm.restart("A");
	f.vm.reconnect("A");
	f.vm.forget("A");
	CHECK(f.sent == std::vector<std::string>{R"({"cmd":"restart","device_id":"A"})",
						  R"({"cmd":"reconnect","device_id":"A"})",
						  R"({"cmd":"forget","device_id":"A"})"});
	CHECK(f.settings.find_display("A")->password_protected.empty());
	CHECK(f.vm.take_settings_dirty());
}

TEST_CASE("helper_restarted clears devices and states")
{
	Fixture f;
	f.vm.apply(parse_event(R"({"event":"devices","devices":[{"device_id":"C","name":"Lobby"}]})"));
	f.display(R"({"event":"display","device_id":"C","state":"live"})");
	f.vm.helper_restarted();
	CHECK(f.vm.rows().empty());
	CHECK_FALSE(f.vm.is_live("C"));
	CHECK_FALSE(f.vm.in_session("C"));
	CHECK(find_row(f.vm.rows(), "C") == nullptr);
}

TEST_CASE("in_session: live, connecting and retrying hold a session; other states do not")
{
	Fixture f;
	CHECK_FALSE(f.vm.in_session("A")); // never reported
	const std::pair<const char *, bool> cases[] = {
		{"connecting", true}, {"live", true},     {"retrying", true}, {"credential", false},
		{"idle", false},      {"offline", false}, {"failed", false},
	};
	for (const auto &c : cases) {
		const std::string state = c.first;
		CAPTURE(state);
		f.display(R"({"event":"display","device_id":"A","state":")" + state + "\"}");
		CHECK(f.vm.in_session("A") == c.second);
	}
}

TEST_CASE("session_restartable: only live and connecting displays take a lead restart")
{
	Fixture f;
	CHECK_FALSE(f.vm.session_restartable("A")); // never reported
	const std::pair<const char *, bool> cases[] = {
		{"connecting", true}, {"live", true},     {"retrying", false}, {"credential", false},
		{"idle", false},      {"offline", false}, {"failed", false},
	};
	for (const auto &c : cases) {
		const std::string state = c.first;
		CAPTURE(state);
		f.display(R"({"event":"display","device_id":"A","state":")" + state + "\"}");
		CHECK(f.vm.session_restartable("A") == c.second);
	}
}

TEST_CASE("groups: locations sorted, displays with no location last, group check state")
{
	Fixture f;
	DisplaySettings &left = f.settings.ensure_display("L", "Left TV");
	left.location = "Meeting Room";
	left.enabled = true;
	f.settings.ensure_display("R", "Right TV").location = "Meeting Room";
	DisplaySettings &sac = f.settings.ensure_display("S", "Studio TV");
	sac.location = "library";
	sac.enabled = true;
	f.vm.apply(parse_event(R"({"event":"devices","devices":[{"device_id":"N","name":"Lobby"}]})"));

	const auto groups = f.vm.groups();
	REQUIRE(groups.size() == 3);
	CHECK(groups[0].location == "library"); // case-insensitive: library < Meeting Room
	CHECK(groups[0].check == GroupCheck::All);
	CHECK(groups[1].location == "Meeting Room");
	CHECK(groups[1].check == GroupCheck::Some);
	REQUIRE(groups[1].displays.size() == 2);
	CHECK(groups[1].displays[0].display_name == "Left TV");
	CHECK(groups[1].displays[0].location == "Meeting Room");
	CHECK(groups[2].location.empty()); // no location: last
	REQUIRE(groups[2].displays.size() == 1);
	CHECK(groups[2].displays[0].device_id == "N");
	CHECK(groups[2].check == GroupCheck::None);

	CHECK(f.vm.locations() == std::vector<std::string>{"library", "Meeting Room"});
}

TEST_CASE("groups: no displays, no groups")
{
	Fixture f;
	CHECK(f.vm.groups().empty());
	CHECK(f.vm.locations().empty());
}

// --- R6: session-only password cache when secure storage is unavailable ---
//
// remember=false below mirrors the real dialog's input on a platform with no
// protector: CredentialDialog only shows and checks "Remember" when
// can_remember_passwords() is true (viewmodel Important-2 ruling), so the UI
// can never send remember=true here. The session cache must still work.

TEST_CASE("R6: a password answer keeps it for later prompts in this model instance even with remember unset")
{
	NoSecretsFixture f;
	f.settings.ensure_display("A", "Roku").enabled = true;
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(f.prompts.size() == 1);
	f.vm.answer_credential("A", "hunter2", false);
	REQUIRE(f.sent.size() == 1);

	// Never written to settings, and settings are not marked dirty for it.
	CHECK(f.settings.find_display("A")->password_protected.empty());
	CHECK_FALSE(f.vm.take_settings_dirty());

	f.display(R"({"event":"display","device_id":"A","state":"live"})");
	f.display(R"({"event":"display","device_id":"A","state":"retrying","error":"timeout"})");
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	CHECK(f.prompts.size() == 1); // answered from the in-memory cache, not re-prompted
	REQUIRE(f.sent.size() == 2);
	const std::string expected_credential = R"({"cmd":"credential","device_id":"A","value":"hunter2"})";
	CHECK(f.sent[1] == expected_credential);
	CHECK(f.settings.find_display("A")->password_protected.empty());
	CHECK_FALSE(f.vm.take_settings_dirty());
}

TEST_CASE("R6: a rejected in-memory password leads to asking with retry")
{
	NoSecretsFixture f;
	f.settings.ensure_display("A", "Roku").enabled = true;
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(f.prompts.size() == 1);
	f.vm.answer_credential("A", "hunter2", false);
	REQUIRE(f.sent.size() == 1);

	// End the episode so the next credential prompt re-checks the cache.
	f.display(R"({"event":"display","device_id":"A","state":"live"})");
	f.display(R"({"event":"display","device_id":"A","state":"retrying","error":"timeout"})");
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	CHECK(f.prompts.size() == 1); // answered from memory, no new prompt
	REQUIRE(f.sent.size() == 2);

	// The receiver rejects it: a second credential prompt in the same episode.
	f.display(R"({"event":"display","device_id":"A","state":"retrying","error":"wrong password"})");
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(f.prompts.size() == 2);
	CHECK(f.prompts[1].retry);
	CHECK(f.settings.find_display("A")->password_protected.empty());
}

TEST_CASE("R6: forget clears the in-memory password")
{
	NoSecretsFixture f;
	f.settings.ensure_display("A", "Roku").enabled = true;
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(f.prompts.size() == 1);
	f.vm.answer_credential("A", "hunter2", false);
	REQUIRE(f.sent.size() == 1);

	f.vm.forget("A");
	const std::string expected_forget = R"({"cmd":"forget","device_id":"A"})";
	CHECK(f.sent.back() == expected_forget);

	f.display(R"({"event":"display","device_id":"A","state":"live"})");
	f.display(R"({"event":"display","device_id":"A","state":"retrying","error":"timeout"})");
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(f.prompts.size() == 2); // no cached value left: a real prompt, not a retry
	CHECK_FALSE(f.prompts[1].retry);
}

TEST_CASE("R6: remember=false with an available protector still caches in memory but never persists")
{
	// Mirrors an operator on Windows who unchecks "Remember": the value must
	// still answer the rest of this OBS session's prompts, but settings are
	// left untouched.
	Fixture f;
	f.settings.ensure_display("A", "Roku").enabled = true;
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	REQUIRE(f.prompts.size() == 1);
	f.vm.answer_credential("A", "hunter2", false);
	CHECK(f.settings.find_display("A")->password_protected.empty());
	CHECK_FALSE(f.vm.take_settings_dirty());

	f.display(R"({"event":"display","device_id":"A","state":"live"})");
	f.display(R"({"event":"display","device_id":"A","state":"retrying","error":"timeout"})");
	f.display(R"({"event":"display","device_id":"A","state":"credential","credential_kind":"password"})");
	CHECK(f.prompts.size() == 1); // answered from the session cache, not re-prompted
	REQUIRE(f.sent.size() == 2);
	CHECK(f.sent[1] == R"({"cmd":"credential","device_id":"A","value":"hunter2"})");
}
