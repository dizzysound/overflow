// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// What the dock shows for each display, and the credential flow (PIN prompts,
// cached password answers). Runs on the UI thread.
#pragma once

#include "airplay/events.hpp"
#include "airplay/secret.hpp"
#include "airplay/settings.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace airplay {

enum class Light { Gray, Yellow, Green, Red };

Light light_for_state(DisplayState state);

struct DisplayRow {
	std::string device_id;
	std::string display_name;
	std::string location; // empty: not in a group
	std::string model;
	std::string ip;
	bool enabled = false;
	bool discovered = false;
	bool manual = false;
	std::string state; // protocol state name; empty when the helper has not reported one
	Light light = Light::Gray;
	std::string status_tooltip;
	bool audio_off = false;
	std::string audio_reason;
	bool needs_receiver_restart = false; // failed with auto-reconnect paused
};

enum class GroupCheck { None, Some, All };

// Displays that share a location, for the dock's group headings.
struct DisplayGroup {
	std::string location; // empty for the displays with no location
	GroupCheck check = GroupCheck::None;
	std::vector<DisplayRow> displays;
};

struct CredentialPrompt {
	std::string device_id;
	std::string display_name;
	std::string kind; // "pin" or "password"
	bool retry = false; // the previous answer was rejected
};

// Controller ruling R6: when the platform's SecretProtector is unavailable (or
// a protect() call fails), the password is kept only in an in-memory map for
// the life of the model / OBS session, and is never written to settings.
class DisplayListModel {
public:
	using CommandSink = std::function<void(std::string)>;
	using PromptSink = std::function<void(const CredentialPrompt &)>;

	DisplayListModel(Settings &settings, const SecretProtector &secrets, CommandSink send, PromptSink prompt);

	void apply(const Event &event);
	// The helper process was (re)started: forget its devices and display states.
	void helper_restarted();

	std::vector<DisplayRow> rows() const;
	// Groups by location, headings sorted by name, the displays with no
	// location last. Within a group, displays keep rows() order.
	std::vector<DisplayGroup> groups() const;
	// Distinct non-empty locations, sorted.
	std::vector<std::string> locations() const;
	std::string display_name_for(const std::string &device_id) const;
	bool is_live(const std::string &device_id) const;
	// True while the helper holds or is (re)establishing a session for the
	// display (live, connecting or retrying): its SETUP lead stays in force.
	bool in_session(const std::string &device_id) const;
	// True while the display is live or connecting: the only states a lead
	// change restarts. A retrying display is not restarted (a restart would
	// reset the helper's reconnect backoff); it takes the new lead when it
	// next connects.
	bool session_restartable(const std::string &device_id) const;

	// value empty = the operator cancelled the prompt. For a password with
	// remember=true: stores the encrypted blob in settings when the protector
	// is available and protect() succeeds (also keeping it in the in-memory
	// map); otherwise keeps it in the in-memory map only (R6).
	void answer_credential(const std::string &device_id, const std::string &value, bool remember);
	void restart(const std::string &device_id);
	void reconnect(const std::string &device_id);
	// Clears the pairing (helper `forget` command) and the saved password, in
	// memory and in settings. Never removes the settings entry.
	void forget(const std::string &device_id);
	// Clears the saved (and in-memory cached) password only, without touching
	// the pairing. Used by the Display settings dialog's "Forget the saved
	// AirPlay password" checkbox.
	void forget_password(const std::string &device_id);

	// True once after the view model changed settings (a saved or cleared password).
	bool take_settings_dirty();

private:
	void on_display_event(const DisplayEvent &event);
	DisplayRow make_row(const std::string &device_id, const DisplaySettings *display) const;

	Settings &settings_;
	const SecretProtector &secrets_;
	CommandSink send_;
	PromptSink prompt_;
	std::map<std::string, Device> devices_;
	std::map<std::string, DisplayEvent> states_;
	std::set<std::string> auto_answered_; // password prompts answered from a cache this episode
	std::set<std::string> prompted_;      // prompts currently shown to the operator
	std::map<std::string, std::string> memory_passwords_; // R6: session-only cache, never written to disk
	bool dirty_ = false;
};

} // namespace airplay
