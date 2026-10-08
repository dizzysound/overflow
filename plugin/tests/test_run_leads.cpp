// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay/ceiling_guard.hpp"
#include "airplay/lead_policy.hpp"

#include <doctest/doctest.h>

#include <string>

using airplay::RunLeads;

namespace {
// Lobby and Studio on 2026-10-04: Auto Wi-Fi displays that started at 197
// and 227 ms, with a 297 ms ceiling, when OBS added 362 ms of audio buffering
// in the middle of Bible class.
const std::string kLobby = "A1B2C3D4E5F6";
const std::string kStudio = "F6E5D4C3B2A1";
constexpr int kSavedLobby = 197;
constexpr int kCeiling = 297;
constexpr int kNeedAfterJump = 400;
} // namespace

TEST_CASE("RunLeads: without a reconnect the saved lead stands")
{
	RunLeads leads;
	CHECK_FALSE(leads.holds(kLobby));
	CHECK(leads.start_lead_ms(kLobby, kSavedLobby) == kSavedLobby);
	CHECK(leads.start_lead_ms(kLobby, 0) == 0); // no saved lead: use the formula
}

TEST_CASE("RunLeads: a reconnect lead holds for the rest of the run")
{
	RunLeads leads;
	leads.set(kLobby, 430);
	CHECK(leads.holds(kLobby));
	// A later session in the same run starts at the reconnect lead...
	CHECK(leads.start_lead_ms(kLobby, kSavedLobby) == 430);
	// ...with no saved lead at all...
	CHECK(leads.start_lead_ms(kLobby, 0) == 430);
	// ...and a higher saved lead still wins.
	CHECK(leads.start_lead_ms(kLobby, 500) == 500);
}

TEST_CASE("RunLeads: one display's reconnect does not move another")
{
	RunLeads leads;
	leads.set(kLobby, 430);
	CHECK_FALSE(leads.holds(kStudio));
	CHECK(leads.start_lead_ms(kStudio, 227) == 227);
}

TEST_CASE("RunLeads: a second reconnect replaces the first, up or down")
{
	RunLeads leads;
	leads.set(kLobby, 430);
	leads.set(kLobby, 465);
	CHECK(leads.start_lead_ms(kLobby, 0) == 465);
	// The need fell between reconnects: the newer, lower lead is the one.
	leads.set(kLobby, 300);
	CHECK(leads.start_lead_ms(kLobby, 0) == 300);
	CHECK(leads.holds(kLobby));
}

// What 2026-10-11 depends on: the reconnect lead must not become the next
// service's starting lead. Auto lowers a saved lead by only 10 ms a session,
// so a saved 430 ms would still be about 390 ms four services later, opening
// the lobby TV that late for nothing.
TEST_CASE("RunLeads: a reconnect lead never becomes the next run's start")
{
	RunLeads leads;
	int saved = kSavedLobby; // what Controller persists, gated on holds()

	// Before the jump, Auto's slide is saved as usual.
	if (!leads.holds(kLobby))
		saved = 210;
	CHECK(saved == 210);

	// 10:57:02: the need passes the ceiling and the guard reconnects higher.
	const int reconnect = airplay::reconnect_lead_ms(airplay::kLeadFloorMs, kNeedAfterJump);
	CHECK(reconnect > kCeiling); // above what the session could reach
	leads.set(kLobby, reconnect);

	// Every 5 s window to the end of the service: the lead holds, the save does not.
	for (int window = 0; window < 12 * 60 / 5; ++window) {
		CHECK(leads.start_lead_ms(kLobby, saved) == reconnect);
		if (!leads.holds(kLobby))
			saved = reconnect;
	}
	CHECK(saved == 210); // the next run still opens at the pre-jump lead

	// OBS restarts, dropping its buffering: a fresh RunLeads, saved lead intact.
	const RunLeads next_run;
	CHECK(next_run.start_lead_ms(kLobby, saved) == 210);
}
