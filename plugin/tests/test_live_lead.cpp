// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay/live_lead.hpp"
#include "airplay/lead_policy.hpp"

#include <doctest/doctest.h>

#include <vector>

using airplay::LiveLead;
using airplay::LiveLeadWindow;

namespace {
constexpr uint64_t kSec = 1000ull * 1000 * 1000;
constexpr uint64_t kMin = 60 * kSec;

// Feeds 5 s windows from t0 to t1 (exclusive) at a fixed effective lead.
int feed(LiveLead &l, uint64_t t0, uint64_t t1, int trouble, int effective, int need = 40)
{
	int last = 0;
	for (uint64_t t = t0; t < t1; t += 5 * kSec) {
		const int target = l.on_window({t, trouble, effective, need});
		if (target > 0)
			last = target;
	}
	return last;
}

// Feeds 5 s windows from t0 to t1 (exclusive) whose effective lead has already
// slid to the current target; returns every target sent, in order.
std::vector<int> follow(LiveLead &l, uint64_t t0, uint64_t t1, int need = 40)
{
	std::vector<int> sent;
	for (uint64_t t = t0; t < t1; t += 5 * kSec) {
		const int target = l.on_window({t, 0, l.target_ms(), need});
		if (target > 0)
			sent.push_back(target);
	}
	return sent;
}
} // namespace

TEST_CASE("LiveLead: two troubled windows in 60 s raise 20 ms, not again until reached")
{
	LiveLead l(false, 70, 130, 0);
	CHECK(l.on_window({5 * kSec, 1, 70, 40}) == 0);
	CHECK(l.on_window({10 * kSec, 1, 70, 40}) == 90);
	// Still sliding up: effective 75 < target 90, more trouble does not stack.
	CHECK(l.on_window({15 * kSec, 1, 75, 40}) == 0);
	CHECK(l.on_window({20 * kSec, 1, 76, 40}) == 0);
	// Reached: the next pair raises again.
	CHECK(l.on_window({120 * kSec, 1, 90, 40}) == 0);
	CHECK(l.on_window({125 * kSec, 1, 90, 40}) == 110);
}

TEST_CASE("LiveLead: troubled windows more than 60 s apart do not raise")
{
	LiveLead l(false, 70, 130, 0);
	CHECK(l.on_window({5 * kSec, 1, 70, 40}) == 0);
	CHECK(l.on_window({70 * kSec, 1, 70, 40}) == 0);
}

TEST_CASE("LiveLead: at the ceiling trouble raises only the next start")
{
	LiveLead l(false, 120, 130, 0);
	CHECK(l.on_window({5 * kSec, 1, 120, 40}) == 0);
	CHECK(l.on_window({10 * kSec, 1, 120, 40}) == 130);
	CHECK(l.on_window({100 * kSec, 1, 130, 40}) == 0);
	CHECK(l.on_window({105 * kSec, 1, 130, 40}) == 0);
	CHECK(l.at_ceiling());
	CHECK(l.next_start_ms() == 150);
}

TEST_CASE("LiveLead: at the ceiling, extra raises are paced and capped at the headroom")
{
	LiveLead l(false, 70, 130, 0);
	// Continuous trouble for 30 minutes; effective follows the target.
	int effective = 70;
	for (uint64_t t = 5 * kSec; t < 30 * kMin; t += 5 * kSec) {
		const int target = l.on_window({t, 1, effective, 40});
		if (target > 0)
			effective = target;
	}
	CHECK(l.next_start_ms() == 190); // 130 + the 60 ms headroom cap

	// A second troubled pair 10 s after the first at-ceiling raise adds nothing.
	LiveLead m(false, 120, 130, 0);
	m.on_window({5 * kSec, 1, 120, 40});
	CHECK(m.on_window({10 * kSec, 1, 120, 40}) == 130);
	m.on_window({100 * kSec, 1, 130, 40});
	m.on_window({105 * kSec, 1, 130, 40});
	CHECK(m.next_start_ms() == 150);
	m.on_window({110 * kSec, 1, 130, 40});
	m.on_window({115 * kSec, 1, 130, 40});
	CHECK(m.next_start_ms() == 150);
}

TEST_CASE("LiveLead raise only: never lowers in session")
{
	LiveLead l(false, 100, 160, 0);
	CHECK(feed(l, 0, 30 * kMin, 0, 100) == 0);
	CHECK(l.target_ms() == 100);
}

TEST_CASE("LiveLead next start: after a raise, the raised target")
{
	LiveLead l(true, 70, 130, 0);
	l.on_window({5 * kSec, 1, 70, 40});
	l.on_window({10 * kSec, 1, 70, 40});
	CHECK(l.next_start_ms() == 90);
}

TEST_CASE("LiveLead dynamic: half-gap steps every 2 clean minutes down to need + 10")
{
	LiveLead l(true, 155, 215, 0);
	CHECK(follow(l, 5 * kSec, 2 * kMin, 70).empty());
	const std::vector<int> want{135, 115, 100, 90, 85, 80};
	CHECK(follow(l, 2 * kMin, 30 * kMin, 70) == want);
	CHECK(l.target_ms() == 80);
}

TEST_CASE("LiveLead dynamic: no step while the previous one is still sliding")
{
	LiveLead l(true, 155, 215, 0);
	follow(l, 5 * kSec, 2 * kMin + 5 * kSec, 70);
	REQUIRE(l.target_ms() == 135);
	// Effective still above the target four minutes later: no further step.
	CHECK(feed(l, 2 * kMin + 5 * kSec, 6 * kMin, 0, 140, 70) == 0);
	CHECK(l.on_window({6 * kMin, 0, 135, 70}) == 115);
}

TEST_CASE("LiveLead dynamic: a small gap steps to the floor, never past it")
{
	LiveLead l(true, 83, 143, 0);
	CHECK(follow(l, 5 * kSec, 10 * kMin, 70) == std::vector<int>{80});
	CHECK(l.target_ms() == 80);
}

TEST_CASE("LiveLead dynamic: the floor never goes below 40")
{
	LiveLead l(true, 60, 120, 0);
	follow(l, 5 * kSec, 30 * kMin, 0);
	CHECK(l.target_ms() == 40);
}

TEST_CASE("LiveLead dynamic: a need spike holds the floor for 10 minutes, then releases it")
{
	LiveLead l(true, 200, 260, 0);
	CHECK(l.on_window({5 * kSec, 0, 200, 150}) == 0);
	follow(l, 10 * kSec, 10 * kMin + 5 * kSec, 40);
	CHECK(l.target_ms() == 160); // 150 + 10 while the spike is in the last 10 minutes
	follow(l, 10 * kMin + 5 * kSec, 30 * kMin, 40);
	CHECK(l.target_ms() == 50);
}

TEST_CASE("LiveLead dynamic: a recent spike keeps holding the floor")
{
	LiveLead l(true, 200, 260, 0);
	follow(l, 5 * kSec, 30 * kMin, 150);
	CHECK(l.target_ms() == 160);
}

TEST_CASE("LiveLead dynamic: trouble after a lowering sets an edge floor and restarts the 2 minute wait")
{
	LiveLead l(true, 100, 160, 0);
	follow(l, 5 * kSec, 2 * kMin + 5 * kSec, 40);
	REQUIRE(l.target_ms() == 80);
	CHECK(l.on_window({3 * kMin, 1, 80, 40}) == 0);
	CHECK(l.on_window({3 * kMin + 5 * kSec, 1, 80, 40}) == 100);
	// Clean again: steps resume 2 minutes after the trouble, down to the edge floor 80 + 10.
	CHECK(follow(l, 3 * kMin + 10 * kSec, 5 * kMin + 5 * kSec, 40).empty());
	CHECK(follow(l, 5 * kMin + 5 * kSec, 30 * kMin, 40) == std::vector<int>{95, 90});
	CHECK(l.target_ms() == 90);
}

TEST_CASE("LiveLead next start: dynamic keeps the lowest lead that ran 10 clean minutes")
{
	LiveLead l(true, 100, 160, 0);
	follow(l, 5 * kSec, 30 * kMin, 40); // 80, 65, 60, 55, 50 by 10 minutes
	CHECK(l.target_ms() == 50);
	CHECK(l.next_start_ms() == 50);
	LiveLead m(true, 100, 160, 0);
	follow(m, 5 * kSec, 8 * kMin, 40); // a clean run under 10 minutes: no best yet
	CHECK(m.next_start_ms() == 100);
}

TEST_CASE("LiveLead next start: raise only lowers 10 ms after a clean 10 minutes, never below recent need + 10")
{
	LiveLead clean(false, 100, 160, 0);
	feed(clean, 5 * kSec, 12 * kMin, 0, 100);
	CHECK(clean.next_start_ms() == 90);
	LiveLead needy(false, 100, 160, 0);
	feed(needy, 5 * kSec, 12 * kMin, 0, 100, 85);
	CHECK(needy.next_start_ms() == 95);
	LiveLead shortrun(false, 100, 160, 0);
	feed(shortrun, 5 * kSec, 3 * kMin, 0, 100);
	CHECK(shortrun.next_start_ms() == 100);
	LiveLead bumpy(false, 100, 160, 0);
	feed(bumpy, 5 * kSec, 12 * kMin, 0, 100);
	bumpy.on_window({12 * kMin, 1, 100, 40});
	CHECK(bumpy.next_start_ms() == 100);
}

TEST_CASE("LiveLead next start: after a raise, later lowerings do not move the saved start")
{
	LiveLead l(true, 100, 160, 0);
	follow(l, 5 * kSec, 2 * kMin + 5 * kSec, 40);
	REQUIRE(l.target_ms() == 80);
	l.on_window({3 * kMin, 1, 80, 40});
	REQUIRE(l.on_window({3 * kMin + 5 * kSec, 1, 80, 40}) == 100);
	follow(l, 3 * kMin + 10 * kSec, 10 * kMin, 40);
	CHECK(l.target_ms() == 90);
	CHECK(l.next_start_ms() == 100);
}

TEST_CASE("LiveLead: floor and recent need follow the window need")
{
	LiveLead f(true, 100, 160, 0);
	f.on_window({5 * kSec, 0, 100, 119});
	CHECK(f.recent_need_ms() == 119);
	CHECK(f.floor_ms() == 129);
}

TEST_CASE("LiveLead: a window stamped before the start does not wrap the elapsed time")
{
	LiveLead s(false, 100, 160, 10 * kMin);
	s.on_window({5 * kSec, 0, 100, 40});
	CHECK(s.next_start_ms() == 100);
	LiveLead d(true, 100, 160, 10 * kMin);
	CHECK(d.on_window({5 * kSec, 0, 100, 40}) == 0);
}
