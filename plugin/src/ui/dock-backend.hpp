// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// What the dock needs from the plugin. Controller implements it; the UI
// smoke tests use a fake. No OBS or Qt types here.
#pragma once

#include "airplay/settings.hpp"
#include "airplay/viewmodel.hpp"

#include <string>
#include <vector>

class DockBackend {
public:
	virtual ~DockBackend() = default;

	virtual std::vector<airplay::DisplayRow> rows() const = 0;
	virtual std::vector<airplay::DisplayGroup> groups() const = 0;
	virtual std::vector<std::string> locations() const = 0;
	virtual const airplay::Settings &settings() const = 0;
	virtual bool running() const = 0;
	virtual std::string status_text() const = 0;
	virtual std::vector<std::string> available_encoders() const = 0;
	// OBS's scene names in the current scene collection, in OBS's order.
	virtual std::vector<std::string> scene_names() const = 0;
	virtual bool can_remember_passwords() const = 0;
	// A plain-text diagnostics report: plugin version and time, helper and
	// output status, global settings, each display's summary, and the
	// recent log lines. Never includes a password or a password_protected
	// blob; those show as "password: saved" or "password: none".
	virtual std::string diagnostics_report() const = 0;

	virtual void set_display_enabled(const std::string &device_id, bool enabled) = 0;
	// Selects or deselects every display in a location at once.
	virtual void set_location_enabled(const std::string &location, bool enabled) = 0;
	virtual void rename_display(const std::string &device_id, const std::string &name) = 0;
	virtual void update_display(const airplay::DisplaySettings &display) = 0;
	virtual void update_global(const airplay::GlobalSettings &global) = 0;
	virtual void add_display(const std::string &name, const std::string &ip, int port, const std::string &location) = 0;
	virtual void user_start() = 0;
	virtual void user_stop() = 0;
	virtual void restart_display(const std::string &device_id) = 0;
	virtual void reconnect_display(const std::string &device_id) = 0;
	// Clears the stored pairing and saved password only (helper `forget`
	// command); the display stays in the list and its settings survive.
	virtual void forget_display(const std::string &device_id) = 0;
	// Removes the display's settings entry entirely. If it was enabled, it
	// drops out of the next selection sent to the helper.
	virtual void remove_display(const std::string &device_id) = 0;
	// Clears the saved (and in-memory cached) AirPlay password without
	// touching the pairing.
	virtual void forget_password(const std::string &device_id) = 0;
	// value empty = the operator cancelled the prompt.
	virtual void answer_credential(const std::string &device_id, const std::string &value, bool remember) = 0;
};
