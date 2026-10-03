// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Ties the pieces together on the Qt UI thread: settings, the helper
// supervisor, the AirPlay output, the display view model, auto-start and the
// idle policy. OBS frontend events come in through handle_frontend_event.
#pragma once

#include "airplay-output.hpp"
#include "airplay/lead_policy.hpp"
#include "airplay/live_lead.hpp"
#include "airplay/log_ring.hpp"
#include "airplay/secret.hpp"
#include "airplay/session_policy.hpp"
#include "airplay/settings.hpp"
#include "airplay/supervisor.hpp"
#include "airplay/viewmodel.hpp"
#include "ui/dock-backend.hpp"

#include <obs-frontend-api.h>

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Controller : public QObject, public DockBackend {
	Q_OBJECT

public:
	struct Paths {
		std::string settings_file;     // obs_module_config_path("settings.json")
		std::string creds_file;        // obs_module_config_path("credentials.json"), owned by the helper
		std::string helper_exe;        // beside obs-overflow.dll
		std::string eld_disabled_path; // a path that does not exist: turns AAC-ELD off
	};

	explicit Controller(Paths paths, QObject *parent = nullptr);
	~Controller() override;

	void handle_frontend_event(enum obs_frontend_event event);
	// Stops the output and the helper (up to 10 s) and saves settings. Idempotent.
	void shutdown();
	// Shown first in the status text, e.g. that the old obs-airplay plugin is
	// still installed beside this one.
	void set_install_warning(const std::string &warning);

	// DockBackend
	std::vector<airplay::DisplayRow> rows() const override;
	std::vector<airplay::DisplayGroup> groups() const override;
	std::vector<std::string> locations() const override;
	const airplay::Settings &settings() const override;
	bool running() const override;
	std::string status_text() const override;
	std::vector<std::string> available_encoders() const override;
	bool can_remember_passwords() const override;
	std::string diagnostics_report() const override;
	void set_display_enabled(const std::string &device_id, bool enabled) override;
	void set_location_enabled(const std::string &location, bool enabled) override;
	void rename_display(const std::string &device_id, const std::string &name) override;
	void update_display(const airplay::DisplaySettings &display) override;
	void update_global(const airplay::GlobalSettings &global) override;
	void add_display(const std::string &name, const std::string &ip, int port, const std::string &location) override;
	void user_start() override;
	void user_stop() override;
	void restart_display(const std::string &device_id) override;
	void reconnect_display(const std::string &device_id) override;
	void forget_display(const std::string &device_id) override;
	void remove_display(const std::string &device_id) override;
	void forget_password(const std::string &device_id) override;
	void answer_credential(const std::string &device_id, const std::string &value, bool remember) override;

signals:
	void rows_changed();
	void status_changed();
	void credential_requested(QString device_id, QString display_name, QString kind, bool retry);

private:
	void start_helper();
	// Starts the helper again if it is missing-then-present or its supervisor
	// gave up (Failed), so auto-start can recover without a settings change.
	void ensure_helper();
	// Checks os_file_exists on the helper exe and updates helper_missing_ /
	// helper_status_ accordingly. Every path that starts or restarts the
	// helper must call this first and skip the supervisor call on false, so
	// a missing exe's clear status message is never overwritten by a spawn
	// failure.
	bool helper_exe_present();
	airplay::HelperLaunch launch_config() const;
	// Each saved display's playout lead as it stands now (lead_policy,
	// spec 2026-09-28 section 6): its fixed TV delay, else the legacy global
	// one, else Auto from the measured OBS audio age, its video need and its
	// late floor.
	std::map<std::string, int> wanted_leads() const;
	// One display's lead from the given video need and late floor, with the
	// OBS audio age as it stands now.
	bool auto_lead(const airplay::DisplaySettings &d) const
	{
		return d.latency_ms == 0 && settings_.global.target_latency_ms == 0;
	}
	int lead_for(const airplay::DisplaySettings &d, int video_need_ms, int late_floor_ms) const;
	// The OBS audio age for leads: the live one once the tap reports, else the
	// last run's.
	uint64_t lead_audio_age_ns() const;
	// Samples the live OBS audio age and records this run's stats, then, for
	// each enabled display in a session, restarts it alone (spec section 6: a
	// session's lead is otherwise fixed) only when (a) the lead from the
	// current audio age, with the video need and late floor its session
	// started with, is at least kLeadRestartStepMs above its lead, or (b) a
	// late-frame raise is pending for it and it has had no late-driven
	// restart in the last 10 minutes. A retrying display is never restarted:
	// it gets the new lead for its next connect. A higher p99 video need alone
	// never restarts a TV; it applies at that display's next session. Called
	// from the 15 s idle timer, once about 3 s after the output starts, and
	// after a late-frame raise.
	void update_leads();
	// Copies this run's max audio age, per-display max p99 and late window
	// counts into settings_. True when anything changed.
	bool note_run_stats();
	// A helper delivery report: raises the display's video need from its
	// p99 (capped at its lead) and, on repeated late windows, raises its late
	// floor and asks update_leads() to restart it (spec section 6).
	void on_delivery(const airplay::DeliveryEvent &dv);
	void on_helper_event(const airplay::Event &event);
	void apply_run_state();
	void resync_displays(bool force);
	void save_settings();
	void note_activity();
	airplay::IdleInputs idle_inputs() const;
	// Logs through obs_log AND appends the same line to log_ring_, so the
	// diagnostics report carries the plugin's own recent log lines as well
	// as the helper's stderr (fed separately, from the supervisor's on_log).
	void log(airplay::LogLevel level, const std::string &message);

	Paths paths_;
	airplay::Settings settings_;
	std::unique_ptr<airplay::SecretProtector> secrets_;
	std::unique_ptr<airplay::HelperSupervisor> supervisor_;
	std::unique_ptr<AirPlayOutput> output_;
	std::unique_ptr<airplay::DisplayListModel> view_;
	airplay::AutoStart autostart_;
	QTimer idle_timer_;
	airplay::LogRing log_ring_;

	bool loaded_ = false;
	bool shut_down_ = false;
	bool ever_started_ = false;
	bool helper_missing_ = false;
	// Set once output_->active() is observed true after a start; cleared once
	// the 15 s timer notices it went away on its own. Distinguishes "it died"
	// (retry now) from "it never came up" (back off retries) in the timer.
	bool output_was_active_ = false;
	uint64_t last_activity_ns_ = 0;
	uint64_t output_stopped_since_ns_ = 0;
	// Next time the 15 s timer may retry a start() that never produced a
	// usable encoder; 0 means "try on the next tick".
	uint64_t start_retry_after_ns_ = 0;
	int launched_fps_ = 0;
	bool displays_sent_ = false;
	std::vector<airplay::DisplaySelection> last_sent_;
	// The lead last sent on set_displays per display: what its current (or
	// next) session runs with. A display in a session keeps it until it
	// restarts; update_leads() raises it only together with a restart, or,
	// for a retrying display, for its next connect.
	std::map<std::string, int> applied_leads_;
	// The video need and late floor each display's current (or next) session
	// started with, captured whenever applied_leads_ takes a new session lead.
	std::map<std::string, int> session_video_need_;
	std::map<std::string, int> session_late_floor_;
	// Largest delivery p99 seen per display this run; saved as last_run_p99_ms.
	std::map<std::string, int> run_p99_max_;
	// Largest live OBS audio age seen this run, ms; saved as last_audio_age_ms.
	int run_audio_age_max_ms_ = 0;
	// Per display: delivery windows with late frames, and late-driven restarts.
	std::map<std::string, airplay::LateWindows> late_windows_;
	// Revision 1.4 live_lead: the helper slides an Auto display's TV delay on
	// set_lead instead of restarting it. One LiveLead per display and session
	// (erased when the session ends), and the lead the last delivery reported.
	bool live_lead_ = false;
	std::map<std::string, airplay::LiveLead> live_leads_;
	std::map<std::string, int> live_effective_;
	std::set<std::string> at_floor_logged_; // dynamic displays whose "at its floor" line was logged this session
	std::map<std::string, airplay::AudioLoss> audio_loss_; // per device: resend requests, for the status text
	// Displays with a late-frame raise that update_leads() has not handled yet.
	std::set<std::string> late_raised_;
	std::string helper_status_ = "not started";
	std::string output_error_;
	std::string install_warning_;
};
