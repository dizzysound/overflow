// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/viewmodel.hpp"

#include <utility>

#include "airplay/commands.hpp"

#include <algorithm>
#include <cctype>

namespace airplay {
namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

} // namespace

Light light_for_state(DisplayState state)
{
	switch (state) {
	case DisplayState::Connecting:
	case DisplayState::Credential:
	case DisplayState::Retrying:
		return Light::Yellow;
	case DisplayState::Live:
		return Light::Green;
	case DisplayState::Failed:
		return Light::Red;
	case DisplayState::Idle:
	case DisplayState::Offline:
	case DisplayState::Unknown:
		break;
	}
	return Light::Gray;
}

DisplayListModel::DisplayListModel(Settings &settings, const SecretProtector &secrets, CommandSink send,
				    PromptSink prompt)
	: settings_(settings), secrets_(secrets), send_(std::move(send)), prompt_(std::move(prompt))
{
}

void DisplayListModel::apply(const Event &event)
{
	if (const auto *devices = std::get_if<DevicesEvent>(&event)) {
		devices_.clear();
		for (const Device &d : devices->devices)
			devices_[d.device_id] = d;
	} else if (const auto *display = std::get_if<DisplayEvent>(&event)) {
		on_display_event(*display);
	}
}

void DisplayListModel::helper_restarted()
{
	devices_.clear();
	states_.clear();
	auto_answered_.clear();
	prompted_.clear();
}

void DisplayListModel::on_display_event(const DisplayEvent &e)
{
	states_[e.device_id] = e;
	if (e.state != DisplayState::Credential) {
		prompted_.erase(e.device_id);
		if (e.state == DisplayState::Live || e.state == DisplayState::Idle)
			auto_answered_.erase(e.device_id);
		return;
	}
	if (prompted_.count(e.device_id))
		return; // the operator is already being asked

	const std::string kind = e.credential_kind.empty() ? "pin" : e.credential_kind;
	const DisplaySettings *display = settings_.find_display(e.device_id);
	// Once a cached answer has been sent this episode, a second credential
	// prompt means the receiver rejected it: ask the operator, marked retry.
	const bool retry = auto_answered_.count(e.device_id) > 0;

	if (kind == "password" && !retry) {
		// R6: the in-memory cache is checked before the settings blob.
		const auto mem_it = memory_passwords_.find(e.device_id);
		if (mem_it != memory_passwords_.end()) {
			auto_answered_.insert(e.device_id);
			send_(credential_command(e.device_id, mem_it->second));
			return;
		}
		if (display && !display->password_protected.empty()) {
			if (auto plain = secrets_.unprotect(display->password_protected)) {
				auto_answered_.insert(e.device_id);
				send_(credential_command(e.device_id, *plain));
				return;
			}
			// unprotect failure counts as no cache: fall through to asking.
		}
	}

	if (retry)
		memory_passwords_.erase(e.device_id); // R6: drop a rejected in-memory value too
	prompted_.insert(e.device_id);
	prompt_(CredentialPrompt{e.device_id, display_name_for(e.device_id), kind, retry});
}

void DisplayListModel::answer_credential(const std::string &device_id, const std::string &value, bool remember)
{
	prompted_.erase(device_id);
	if (value.empty())
		return; // the operator cancelled
	send_(credential_command(device_id, value));
	const auto it = states_.find(device_id);
	const bool is_password = it != states_.end() && it->second.credential_kind == "password";
	if (is_password) {
		auto_answered_.insert(device_id); // a second prompt now means "rejected"
		// Always keep the password for the rest of this OBS session, whether or
		// not "Remember" was checked and regardless of whether secure storage is
		// available: without this, an operator who leaves Remember unchecked (or
		// whose platform has no protector) retypes the password on every prompt.
		memory_passwords_[device_id] = value;
		if (remember && secrets_.available()) {
			// Persist only when asked to, and only when it can be protected;
			// otherwise the value lives in memory only, for this session.
			if (auto blob = secrets_.protect(value)) {
				std::string &saved =
					settings_.ensure_display(device_id, display_name_for(device_id)).password_protected;
				if (!saved.empty())
					pending_discards_.push_back(saved); // the replaced password's OS-store item
				saved = *blob;
				dirty_ = true;
			}
		}
	}
}

void DisplayListModel::restart(const std::string &device_id)
{
	send_(restart_command(device_id));
}

void DisplayListModel::reconnect(const std::string &device_id)
{
	send_(reconnect_command(device_id));
}

void DisplayListModel::forget(const std::string &device_id)
{
	send_(forget_command(device_id));
	auto_answered_.erase(device_id);
	forget_password(device_id);
}

void DisplayListModel::forget_password(const std::string &device_id)
{
	memory_passwords_.erase(device_id); // R6: clear the in-memory copy too
	if (DisplaySettings *display = settings_.find_display(device_id)) {
		if (!display->password_protected.empty()) {
			pending_discards_.push_back(display->password_protected);
			display->password_protected.clear();
			dirty_ = true;
		}
	}
}

std::vector<std::string> DisplayListModel::take_pending_discards()
{
	return std::exchange(pending_discards_, {});
}

bool DisplayListModel::take_settings_dirty()
{
	const bool d = dirty_;
	dirty_ = false;
	return d;
}

std::string DisplayListModel::display_name_for(const std::string &device_id) const
{
	const DisplaySettings *display = settings_.find_display(device_id);
	if (display && !display->display_name.empty())
		return display->display_name;
	const auto it = devices_.find(device_id);
	if (it != devices_.end() && !it->second.name.empty())
		return it->second.name;
	return device_id;
}

bool DisplayListModel::is_live(const std::string &device_id) const
{
	const auto it = states_.find(device_id);
	return it != states_.end() && it->second.state == DisplayState::Live;
}

bool DisplayListModel::in_session(const std::string &device_id) const
{
	const auto it = states_.find(device_id);
	if (it == states_.end())
		return false;
	const DisplayState st = it->second.state;
	return st == DisplayState::Live || st == DisplayState::Connecting || st == DisplayState::Retrying;
}

bool DisplayListModel::session_restartable(const std::string &device_id) const
{
	const auto it = states_.find(device_id);
	if (it == states_.end())
		return false;
	const DisplayState st = it->second.state;
	return st == DisplayState::Live || st == DisplayState::Connecting;
}

DisplayRow DisplayListModel::make_row(const std::string &device_id, const DisplaySettings *display) const
{
	DisplayRow row;
	row.device_id = device_id;
	row.display_name = display_name_for(device_id);
	const auto dev = devices_.find(device_id);
	row.discovered = dev != devices_.end();
	if (row.discovered) {
		row.model = dev->second.model;
		row.ip = dev->second.ip;
	}
	if (display) {
		row.location = display->location;
		row.enabled = display->enabled;
		row.manual = !display->manual_ip.empty();
		if (row.manual)
			row.ip = display->manual_ip;
	}

	const auto st = states_.find(device_id);
	if (st == states_.end()) {
		row.status_tooltip = row.discovered ? "Discovered at " + row.ip
				     : row.manual  ? "Manual address " + row.ip
						   : "Not discovered on the network";
		return row;
	}
	const DisplayEvent &e = st->second;
	row.state = e.state_text;
	row.light = light_for_state(e.state);
	row.status_tooltip = e.state_text;
	if (e.state == DisplayState::Credential) {
		row.status_tooltip += e.credential_kind == "password" ? " (password)" : " (PIN)";
		// Minor 2: cancelling the prompt leaves the display stuck here (the
		// helper only emits on change, so nothing re-prompts); tell the
		// operator the way out.
		row.status_tooltip += "\nChoose Restart to enter the code again.";
	}
	if (!e.error.empty())
		row.status_tooltip += ": " + e.error;
	if (e.state == DisplayState::Live && e.audio == "off") {
		row.audio_off = true;
		row.audio_reason = e.audio_reason;
	}
	if (e.state == DisplayState::Failed && e.error.find("auto-reconnect paused") != std::string::npos) {
		row.needs_receiver_restart = true;
		row.status_tooltip += "\nRestart the receiver, then choose Reconnect.";
	}
	return row;
}

std::vector<DisplayRow> DisplayListModel::rows() const
{
	std::vector<DisplayRow> rows;
	std::set<std::string> seen;
	for (const DisplaySettings &r : settings_.displays) {
		rows.push_back(make_row(r.device_id, &r));
		seen.insert(r.device_id);
	}
	for (const auto &kv : devices_)
		if (!seen.count(kv.first))
			rows.push_back(make_row(kv.first, nullptr));
	std::sort(rows.begin(), rows.end(), [](const DisplayRow &a, const DisplayRow &b) {
		const std::string la = lower(a.display_name), lb = lower(b.display_name);
		return la != lb ? la < lb : a.device_id < b.device_id;
	});
	return rows;
}

std::vector<DisplayGroup> DisplayListModel::groups() const
{
	std::vector<DisplayGroup> groups;
	DisplayGroup ungrouped;
	for (DisplayRow &row : rows()) {
		if (row.location.empty()) {
			ungrouped.displays.push_back(std::move(row));
			continue;
		}
		auto it = std::find_if(groups.begin(), groups.end(),
					[&](const DisplayGroup &g) { return g.location == row.location; });
		if (it == groups.end()) {
			groups.push_back(DisplayGroup{row.location, GroupCheck::None, {}});
			it = groups.end() - 1;
		}
		it->displays.push_back(std::move(row));
	}
	std::sort(groups.begin(), groups.end(), [](const DisplayGroup &a, const DisplayGroup &b) {
		const std::string la = lower(a.location), lb = lower(b.location);
		return la != lb ? la < lb : a.location < b.location;
	});
	if (!ungrouped.displays.empty())
		groups.push_back(std::move(ungrouped));
	for (DisplayGroup &g : groups) {
		const auto on = std::count_if(g.displays.begin(), g.displays.end(), [](const DisplayRow &r) { return r.enabled; });
		g.check = on == 0 ? GroupCheck::None
			  : static_cast<size_t>(on) == g.displays.size() ? GroupCheck::All
									  : GroupCheck::Some;
	}
	return groups;
}

std::vector<std::string> DisplayListModel::locations() const
{
	std::vector<std::string> out;
	for (const DisplaySettings &d : settings_.displays)
		if (!d.location.empty() && std::find(out.begin(), out.end(), d.location) == out.end())
			out.push_back(d.location);
	std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) { return lower(a) < lower(b); });
	return out;
}

} // namespace airplay
