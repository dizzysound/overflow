// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "controller.hpp"

#include "airplay/commands.hpp"
#include "airplay/lead_policy.hpp"
#include "airplay/redact.hpp"
#include "airplay/settings_store.hpp"

#include <plugin-support.h>
#include <util/platform.h>

#include <QMetaObject>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <sstream>

namespace {

int obs_level(airplay::LogLevel level)
{
	switch (level) {
	case airplay::LogLevel::Debug:
		return LOG_DEBUG;
	case airplay::LogLevel::Warning:
		return LOG_WARNING;
	case airplay::LogLevel::Error:
		return LOG_ERROR;
	case airplay::LogLevel::Info:
		break;
	}
	return LOG_INFO;
}

// Matches airplay::Settings' own device-id normalization (settings.cpp):
// ASCII uppercase, so a guard here compares on the same footing as the
// uppercase-invariant ids that settings_/view_ actually store.
std::string to_upper_ascii(const std::string &s)
{
	std::string out = s;
	for (char &c : out)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return out;
}

// 60 s: how long the 15 s idle timer backs off retrying a start() that never
// produced a usable encoder, so the UI thread isn't churning on encoder init
// every tick. Explicit user actions (Start, settings changes, streaming or
// recording starting) bypass this by calling apply_run_state() directly.
constexpr uint64_t kStartBackoffNs = 60ull * 1000 * 1000 * 1000;

} // namespace

Controller::Controller(Paths paths, QObject *parent)
	: QObject(parent), paths_(std::move(paths)), secrets_(airplay::make_platform_protector())
{
	std::string warning;
	settings_ = airplay::load_settings_file(paths_.settings_file, &warning);
	if (!warning.empty())
		log(airplay::LogLevel::Warning, warning);
	// The learned video need may come down once per start, when the whole
	// last run needed well under it (spec section 6); the late floor only
	// after a run without late windows.
	for (airplay::DisplaySettings &d : settings_.displays) {
		d.video_need_ms = airplay::learn_video_need_at_start(d.video_need_ms, d.last_run_p99_ms);
		d.late_floor_ms = airplay::learn_late_floor_at_start(d.late_floor_ms, d.last_run_late_windows);
	}
	// First run after install: start audio TVs from the legacy measured audio
	// age, so they do not restart once the live age arrives.
	settings_.global.last_audio_age_ms =
		airplay::seed_audio_age_ms(settings_.global.last_audio_age_ms, settings_.global.auto_latency_ms);
	autostart_.set_options(airplay::autostart_options(settings_.global));

	airplay::SupervisorCallbacks cb;
	cb.on_event = [this](const airplay::Event &event) {
		QMetaObject::invokeMethod(this, [this, event] { on_helper_event(event); }, Qt::QueuedConnection);
	};
	// Runs on helper reader threads; log() itself is thread-safe (LogRing
	// takes a mutex briefly, and obs_log is safe to call off the UI thread).
	cb.on_log = [this](airplay::LogLevel level, const std::string &message) { log(level, message); };
	cb.on_status = [this](airplay::HelperStatus status, const std::string &detail) {
		std::string text = airplay::helper_status_name(status);
		if (!detail.empty())
			text += ": " + detail;
		QMetaObject::invokeMethod(
			this,
			[this, text] {
				helper_status_ = text;
				emit status_changed();
			},
			Qt::QueuedConnection);
	};
	supervisor_ = std::make_unique<airplay::HelperSupervisor>(std::move(cb), [] { return os_gettime_ns(); });
	output_ = std::make_unique<AirPlayOutput>(supervisor_->media());
	view_ = std::make_unique<airplay::DisplayListModel>(
		settings_, *secrets_, [this](std::string command) { supervisor_->send_command(std::move(command)); },
		[this](const airplay::CredentialPrompt &p) {
			// Task 10 parked minor: a credential prompt for a device_id that
			// is neither saved nor discovered is not the operator's problem
			// to answer. Log it and drop it instead of raising a dialog.
			bool known = settings_.find_display(p.device_id) != nullptr;
			if (!known) {
				// DisplayRow::device_id is stored uppercase; compare on the
				// same footing rather than assuming the helper's id already
				// matches that case.
				const std::string upper_id = to_upper_ascii(p.device_id);
				for (const airplay::DisplayRow &row : view_->rows()) {
					if (row.device_id == upper_id) {
						known = true;
						break;
					}
				}
			}
			if (!known) {
				log(airplay::LogLevel::Warning,
				    "credential prompt for unknown device " + p.device_id + " ignored");
				return;
			}
			emit credential_requested(QString::fromStdString(p.device_id),
						   QString::fromStdString(p.display_name), QString::fromStdString(p.kind),
						   p.retry);
		});

	idle_timer_.setInterval(15000);
	connect(&idle_timer_, &QTimer::timeout, this, [this] {
		if (!autostart_.should_run()) {
			output_was_active_ = false; // not our concern while stopped by policy
			resync_displays(false);
			return;
		}
		if (output_->active()) {
			output_was_active_ = true;
			update_leads();
			resync_displays(false);
			return;
		}
		if (output_was_active_) {
			// Task 15 review minor 1: the output stopped on its own (an
			// encoder error reached full_stop); stop() cleans it up and
			// apply_run_state() tries again, immediately, since it had
			// been running before.
			output_was_active_ = false;
			log(airplay::LogLevel::Warning, "AirPlay output stopped unexpectedly; restarting it");
			output_->stop();
			output_stopped_since_ns_ = os_gettime_ns();
			apply_run_state();
			return;
		}
		// It never became active after a start attempt (most likely: no
		// usable encoder). Don't retry start() from here more often than
		// every 60 s; apply_run_state() logs the failure when it happens.
		const uint64_t now = os_gettime_ns();
		if (now < start_retry_after_ns_) {
			resync_displays(false);
			return;
		}
		start_retry_after_ns_ = now + kStartBackoffNs;
		ensure_helper(); // a helper that was missing or failed may be back
		apply_run_state();
	});
}

Controller::~Controller()
{
	shutdown();
}

void Controller::handle_frontend_event(enum obs_frontend_event event)
{
	if (shut_down_)
		return;
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		loaded_ = true;
		read_program_scene();
		start_helper();
		autostart_.obs_loaded();
		note_activity();
		apply_run_state();
		idle_timer_.start();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		autostart_.streaming(event == OBS_FRONTEND_EVENT_STREAMING_STARTED);
		note_activity();
		apply_run_state();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		autostart_.recording(event == OBS_FRONTEND_EVENT_RECORDING_STARTED);
		note_activity();
		apply_run_state();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		read_program_scene();
		note_activity();
		resync_displays(false);
		emit rows_changed();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		shutdown();
		break;
	default:
		break;
	}
}

void Controller::shutdown()
{
	if (shut_down_)
		return;
	shut_down_ = true;
	idle_timer_.stop();
	if (output_)
		output_->stop();
	note_run_stats();
	save_settings();
	if (supervisor_)
		supervisor_->stop(std::chrono::seconds(10));
}

void Controller::start_helper()
{
	if (!helper_exe_present())
		return;
	launched_fps_ = AirPlayOutput::current_fps();
	supervisor_->start(launch_config());
}

void Controller::ensure_helper()
{
	if (!loaded_ || shut_down_)
		return;
	const airplay::HelperStatus st = supervisor_->status();
	if (!helper_missing_ && st != airplay::HelperStatus::Failed && st != airplay::HelperStatus::Stopped)
		return; // starting, running or restarting on its own
	if (!helper_exe_present())
		return;
	launched_fps_ = AirPlayOutput::current_fps();
	supervisor_->restart(launch_config()); // starts it when stopped or failed
}

bool Controller::helper_exe_present()
{
	if (os_file_exists(paths_.helper_exe.c_str())) {
		helper_missing_ = false;
		return true;
	}
	// Only (re-)announce on the transition into "missing", so a spawn that
	// would otherwise follow never gets the chance to overwrite this clear
	// message with a spawn-failure status (see the header comment).
	if (!helper_missing_) {
		helper_missing_ = true;
		helper_status_ = "overflow-helper not found at " + paths_.helper_exe + "; reinstall the plugin";
		log(airplay::LogLevel::Error, helper_status_);
		emit status_changed();
	}
	return false;
}

airplay::HelperLaunch Controller::launch_config() const
{
	airplay::HelperLaunch l;
	l.exe = paths_.helper_exe;
	l.creds_path = paths_.creds_file;
	l.fps = launched_fps_ > 0 ? launched_fps_ : 30;
	l.timing = settings_.global.timing;
	// Explicit only: per-display leads travel on set_displays (latency_ms).
	l.target_latency_ms = settings_.global.target_latency_ms;
	if (!settings_.global.eld_encoder)
		l.eld_encoder_path = paths_.eld_disabled_path; // absent encoder: AAC-ELD-only displays get video only
	l.debug = settings_.global.verbose_helper_log;
	return l;
}

uint64_t Controller::lead_audio_age_ns() const
{
	// The live OBS audio age once the tap has reported; until then (each
	// output start resets it) the last run's max, so a TV that connects
	// early does not start video-only and restart once audio is measured.
	const uint64_t live = output_->audio_tap_active() ? output_->audio_age_ns() : 0;
	return live > 0 ? live : static_cast<uint64_t>(settings_.global.last_audio_age_ms) * 1000000ull;
}

int Controller::lead_for(const airplay::DisplaySettings &d, int video_need_ms, int late_floor_ms) const
{
	airplay::LeadInputs in;
	in.fixed_ms = d.latency_ms;
	in.global_fixed_ms = settings_.global.target_latency_ms;
	in.audio_on = d.audio_enabled;
	in.wifi = d.wifi_tolerant;
	in.audio_age_ns = lead_audio_age_ns();
	in.video_need_ms = video_need_ms;
	in.late_floor_ms = late_floor_ms;
	in.saved_lead_ms = live_lead_ && auto_lead(d) ? d.saved_lead_ms : 0;
	return airplay::display_lead_ms(in);
}

std::map<std::string, int> Controller::wanted_leads() const
{
	std::map<std::string, int> out;
	for (const airplay::DisplaySettings &d : settings_.displays)
		out[d.device_id] = lead_for(d, d.video_need_ms, d.late_floor_ms);
	return out;
}

bool Controller::note_run_stats()
{
	bool changed = false;
	if (run_audio_age_max_ms_ > 0 && run_audio_age_max_ms_ != settings_.global.last_audio_age_ms) {
		settings_.global.last_audio_age_ms = run_audio_age_max_ms_;
		changed = true;
	}
	for (airplay::DisplaySettings &d : settings_.displays) {
		const auto run = run_p99_max_.find(d.device_id);
		if (run != run_p99_max_.end() && run->second != d.last_run_p99_ms) {
			d.last_run_p99_ms = run->second;
			changed = true;
		}
		// Only a display that delivered this run: one that never streamed
		// keeps the count (and so the late floor) it had.
		const auto late = late_windows_.find(d.device_id);
		if (late != late_windows_.end() && late->second.run_late_windows() != d.last_run_late_windows) {
			d.last_run_late_windows = late->second.run_late_windows();
			changed = true;
		}
	}
	return changed;
}

void Controller::update_leads()
{
	if (output_->audio_tap_active()) {
		const uint64_t age_ns = output_->audio_age_ns();
		if (age_ns > 0)
			run_audio_age_max_ms_ = std::max(run_audio_age_max_ms_, static_cast<int>(std::min<uint64_t>(
											  age_ns / 1000000ull, airplay::kLeadCeilingMs)));
	}
	bool changed = note_run_stats();
	const uint64_t now = os_gettime_ns();
	std::vector<std::string> restarts;
	for (airplay::DisplaySettings &d : settings_.displays) {
		const bool late_raised = late_raised_.erase(d.device_id) > 0;
		// Only a display in a session is considered: any other one picks up its
		// wanted lead from resync_displays() below before it next connects.
		const auto applied = applied_leads_.find(d.device_id);
		if (!d.enabled || applied == applied_leads_.end() || !view_->in_session(d.device_id))
			continue;
		if (live_lead_ && auto_lead(d)) {
			// The slide replaces restart-to-raise. A display with no LiveLead is
			// not in a live slide session (it dropped to Retrying, or has not
			// reported yet): its next connect starts from the saved lead, never
			// by a restart. A Live display with a LiveLead is left untouched.
			if (live_leads_.find(d.device_id) == live_leads_.end())
				applied->second = lead_for(d, d.video_need_ms, d.late_floor_ms);
			continue;
		}
		// Spec section 6 fixes a session's lead. The video need and late floor
		// the session started with stand in for the current ones, so only a
		// rise in the OBS audio age can call for a restart here.
		const auto need = session_video_need_.find(d.device_id);
		const auto floor = session_late_floor_.find(d.device_id);
		const int want_session =
			lead_for(d, need != session_video_need_.end() ? need->second : d.video_need_ms,
				 floor != session_late_floor_.end() ? floor->second : d.late_floor_ms);
		const bool audio_rise = airplay::lead_needs_restart(applied->second, want_session);
		const bool restartable = view_->session_restartable(d.device_id);
		// The late floor is saved either way; a restart for it at most once per
		// 10 minutes, so late frames every service cannot keep blanking a TV.
		bool late_restart = false;
		if (late_raised && restartable) {
			late_restart = late_windows_[d.device_id].take_restart(now);
			if (!late_restart)
				log(airplay::LogLevel::Info, d.display_name + ": TV delay raise to " +
								     std::to_string(d.late_floor_ms) +
								     " ms waits for its next session (restarted for late frames "
								     "within the last 10 minutes)");
		}
		if (!audio_rise && !late_restart && !(late_raised && !restartable))
			continue;
		// A new session takes everything learned so far, p99 need included.
		const int want = std::max(lead_for(d, d.video_need_ms, d.late_floor_ms), applied->second);
		if (want == applied->second)
			continue;
		log(airplay::LogLevel::Info, d.display_name + ": TV delay " + std::to_string(applied->second) + " -> " +
						     std::to_string(want) + " ms" +
						     (restartable ? (late_restart ? " (late frames)" : " (OBS audio age)")
								  : " at its next connect (retrying)"));
		applied->second = want;
		session_video_need_[d.device_id] = d.video_need_ms;
		session_late_floor_[d.device_id] = d.late_floor_ms;
		// Never restart a retrying display: that resets the helper's reconnect
		// backoff. set_displays below gives it the new lead for its next connect.
		if (restartable)
			restarts.push_back(d.device_id);
	}
	if (changed || !restarts.empty())
		save_settings(); // this run's audio age, p99 and late windows, and any need or floor raised on the way here
	// set_displays carries the raised leads first, so each restart reconnects with its new one.
	resync_displays(false);
	for (const std::string &id : restarts)
		supervisor_->send_command(airplay::restart_command(id));
}

void Controller::on_delivery(const airplay::DeliveryEvent &dv)
{
	airplay::DisplaySettings *d = settings_.find_display(dv.device_id);
	if (!d)
		return;
	const uint64_t now = os_gettime_ns();
	airplay::AudioLoss &loss = audio_loss_[d->device_id];
	const bool loss_seen = loss.seen();
	const std::string text_before = airplay::audio_loss_text(loss, now);
	loss.record(now, dv.audio_lost, dv.audio_resent, dv.audio_dropped);
	if (dv.audio_lost > 0 || dv.audio_dropped > 0)
		log(airplay::LogLevel::Info, d->display_name + ": in 5 s, " + std::to_string(dv.audio_lost) +
						     " audio packets lost (" + std::to_string(dv.audio_resent) +
						     " resent), " + std::to_string(dv.audio_dropped) +
						     " audio frames dropped late, at TV delay " + std::to_string(dv.lead_ms) +
						     " ms");
	if (dv.audio_jumps > 0)
		log(airplay::LogLevel::Warning, d->display_name + ": in 5 s, " + std::to_string(dv.audio_jumps) +
							" audio timeline jumps (the receiver has to conceal each one)");
	// The status text shows the last minute's losses: repaint when that changes.
	if (!loss_seen || airplay::audio_loss_text(loss, now) != text_before)
		emit rows_changed();
	// A p99 above the display's lead means late frames, which the late path
	// below handles: cap it at the lead so one outlier window cannot push
	// the need (and every later session's lead) toward 2 s. The raised need
	// applies at the display's next session, never by restarting this one.
	const auto applied = applied_leads_.find(d->device_id);
	// A slid-up lead (dv.lead_ms above the session-start lead) raises the cap too.
	const int cap = applied != applied_leads_.end() ? std::max(applied->second, dv.lead_ms) : std::max(dv.lead_ms, 0);
	const int p99 = cap > 0 ? std::min(dv.p99_ms, cap) : dv.p99_ms;
	int &run = run_p99_max_[d->device_id];
	run = std::max(run, p99);
	d->video_need_ms = airplay::raise_video_need(d->video_need_ms, dv.p99_ms, cap);

	const int audio_need =
		d->audio_enabled ? static_cast<int>(output_->audio_age_ns() / 1000000ull) + airplay::kEldFrameMs : 0;
	const int need = std::max(dv.p99_ms, audio_need); // this window's need (spec 10), not the learned video need
	const int trouble = airplay::lead_trouble(dv.late, dv.audio_lost, dv.audio_resent, dv.audio_dropped);
	if (guard_ceiling(*d, dv, need, trouble)) {
		emit rows_changed();
		return;
	}

	// A helper that slides the lead (live_lead) moves an Auto display's TV
	// delay with set_lead instead of restarting it; no late-window restart.
	if (live_lead_ && auto_lead(*d) && dv.lead_ceiling_ms > 0) {
		const bool dynamic = d->lead_mode == airplay::kLeadModeDynamic;
		auto it = live_leads_.find(d->device_id);
		if (it == live_leads_.end() || it->second.ceiling_ms() != dv.lead_ceiling_ms ||
		    it->second.dynamic() != dynamic) {
			live_leads_.erase(d->device_id);
			at_floor_logged_.erase(d->device_id);
			it = live_leads_
				     .emplace(d->device_id,
					      airplay::LiveLead(dynamic, dv.lead_target_ms, dv.lead_ceiling_ms, now))
				     .first;
		}
		live_effective_[d->device_id] = dv.lead_ms;
		const int prev_target = it->second.target_ms();
		const int target = it->second.on_window({now, trouble, dv.lead_ms, need});
		if (target > 0) {
			log(airplay::LogLevel::Info, d->display_name + ": TV delay target " + std::to_string(target) +
							     " ms (from " + std::to_string(dv.lead_ms) + "; floor " +
							     std::to_string(it->second.floor_ms()) + ", recent need " +
							     std::to_string(it->second.recent_need_ms()) + ", window p99 " +
							     std::to_string(dv.p99_ms) + ")");
			supervisor_->send_command(airplay::set_lead_command(d->device_id, target));
			// Once per session, say so when a lowering lands on the floor.
			if (it->second.dynamic() && target < prev_target && target <= it->second.floor_ms() &&
			    at_floor_logged_.insert(d->device_id).second)
				log(airplay::LogLevel::Info, d->display_name + ": TV delay at its floor " +
								     std::to_string(it->second.floor_ms()) + " ms (recent need " +
								     std::to_string(it->second.recent_need_ms()) + ")");
		}
		// Saved on a raise, or when it moved by 5 ms or more: not every 5 s window.
		const int next = it->second.next_start_ms();
		if (next != d->saved_lead_ms && (target > prev_target || std::abs(next - d->saved_lead_ms) >= 5)) {
			d->saved_lead_ms = next;
			save_settings();
		}
		emit rows_changed();
		return;
	}

	// Spec section 6: repeated late frames. A delivery window with any late
	// frame counts once; more than 3 such windows in 60 s raise the lead.
	// Late video, audio dropped as late, and unrecovered audio loss all count.
	if (!late_windows_[d->device_id].record(now, trouble))
		return;
	if (d->latency_ms > 0 || settings_.global.target_latency_ms > 0)
		return; // a fixed TV delay is the operator's choice; Auto alone learns
	// The late floor holds the raised lead across restarts and runs, until a
	// run without late windows (learn_late_floor_at_start).
	const int lead = applied != applied_leads_.end() ? applied->second : wanted_leads().at(d->device_id);
	d->late_floor_ms = std::max(d->late_floor_ms, airplay::late_floor_after_raise(lead));
	log(airplay::LogLevel::Info, d->display_name + ": late video or audio dropouts in more than " +
					     std::to_string(airplay::LateWindows::kWindowsToRaise) +
					     " delivery windows in 60 s; TV delay floor " + std::to_string(d->late_floor_ms) +
					     " ms");
	late_raised_.insert(d->device_id);
	update_leads(); // restarts it now, if it is live and was not restarted for this in 10 minutes
}

bool Controller::guard_ceiling(airplay::DisplaySettings &d, const airplay::DeliveryEvent &dv, int need_ms, int trouble)
{
	// A helper without live_lead reports no ceiling: its lead is fixed for the session.
	const int ceiling = dv.lead_ceiling_ms > 0 ? dv.lead_ceiling_ms : dv.lead_ms;
	const bool fixed = !auto_lead(d);
	const bool may_reconnect =
		live_lead_ && !fixed && dv.lead_ceiling_ms > 0 && view_->session_restartable(d.device_id);
	// What a fresh session would start at now, from the live audio age.
	airplay::DisplaySettings fresh = d;
	fresh.saved_lead_ms = 0;
	const int wanted = lead_for(fresh, d.video_need_ms, d.late_floor_ms);

	airplay::CeilingGuard &guard = ceiling_guards_[d.device_id];
	const bool was_exhausted = guard.exhausted();
	const airplay::CeilingGuard::Step step =
		guard.on_window({os_gettime_ns(), trouble, need_ms, ceiling}, may_reconnect, wanted);
	if (step.warning_started || (guard.warning() && guard.exhausted() && !was_exhausted))
		log(airplay::LogLevel::Warning, airplay::ceiling_warning_text(d.display_name, guard, fixed));
	if (step.warning_cleared)
		log(airplay::LogLevel::Info, d.display_name + ": TV delay " + std::to_string(dv.lead_ms) +
						     " ms covers the need again (" + std::to_string(need_ms) + " ms)");
	if (step.reconnect_ms <= 0)
		return false;

	log(airplay::LogLevel::Warning,
	    d.display_name + ": reconnecting at TV delay " + std::to_string(step.reconnect_ms) + " ms (need about " +
		    std::to_string(guard.need_ms()) + " ms, above its session's ceiling " + std::to_string(ceiling) +
		    " ms; reconnect " + std::to_string(guard.reconnects()) + " of " +
		    std::to_string(airplay::CeilingGuard::kMaxReconnects) + " this run)");
	// The saved lead is where an Auto display's next session starts (lead_for);
	// Auto dynamic lowers it again once the need falls.
	d.saved_lead_ms = step.reconnect_ms;
	applied_leads_[d.device_id] = step.reconnect_ms;
	live_leads_.erase(d.device_id);
	live_effective_.erase(d.device_id);
	at_floor_logged_.erase(d.device_id);
	save_settings();
	// set_displays carries the new lead first, so the restart reconnects with it.
	resync_displays(false);
	supervisor_->send_command(airplay::restart_command(d.device_id));
	return true;
}

std::vector<std::string> Controller::ceiling_warnings() const
{
	std::vector<std::string> out;
	for (const airplay::DisplaySettings &d : settings_.displays) {
		const auto it = ceiling_guards_.find(d.device_id);
		if (d.enabled && it != ceiling_guards_.end() && it->second.warning() && view_->in_session(d.device_id))
			out.push_back(airplay::ceiling_warning_text(d.display_name, it->second, !auto_lead(d)));
	}
	return out;
}

void Controller::on_helper_event(const airplay::Event &event)
{
	if (shut_down_)
		return;
	// Neither changes what the dock shows, so neither repaints it.
	if (const auto *k = std::get_if<airplay::KeyframeEvent>(&event)) {
		if (output_->request_keyframe())
			log(airplay::LogLevel::Debug, "keyframe for " + k->device_id + " (" + k->reason + ")");
		return;
	}
	if (const auto *dv = std::get_if<airplay::DeliveryEvent>(&event)) {
		on_delivery(*dv);
		return;
	}
	// A display that left the live state ended its session: the next one
	// starts a fresh LiveLead from the lead the helper reports.
	const airplay::DisplayEvent *session_ended = nullptr;
	if (const auto *de = std::get_if<airplay::DisplayEvent>(&event)) {
		if (de->state != airplay::DisplayState::Live) {
			if (live_leads_.erase(de->device_id) > 0)
				session_ended = de;
			at_floor_logged_.erase(de->device_id);
			live_effective_.erase(de->device_id);
		}
	}
	if (const auto *rd = std::get_if<airplay::ReadyEvent>(&event)) {
		// A new helper process: it knows nothing yet.
		live_lead_ = rd->has("live_lead");
		live_leads_.clear();
		at_floor_logged_.clear();
		live_effective_.clear();
		view_->helper_restarted();
		resync_displays(true);
	}
	view_->apply(event);
	if (session_ended) {
		// A slid-up Auto display that dropped (typically to Retrying) reconnects
		// at the saved lead, not the stale session-start one: update applied and
		// send set_displays before the helper's first retry. No restart, and
		// sending commands raises no synchronous DisplayEvent.
		const airplay::DisplaySettings *sd = settings_.find_display(session_ended->device_id);
		const auto applied = applied_leads_.find(session_ended->device_id);
		if (sd && live_lead_ && auto_lead(*sd) && applied != applied_leads_.end()) {
			applied->second = lead_for(*sd, sd->video_need_ms, sd->late_floor_ms);
			resync_displays(false);
		}
	}
	emit rows_changed();
	// A helper that just became ready may unblock an output held back
	// because the helper was missing or failed (Minor 4's gate).
	if (std::holds_alternative<airplay::ReadyEvent>(event) && autostart_.should_run() && !output_->active())
		apply_run_state();
}

void Controller::apply_run_state()
{
	const bool want = autostart_.should_run();
	if (want && !output_->active()) {
		const int fps = AirPlayOutput::current_fps();
		if (loaded_ && fps != launched_fps_ && helper_exe_present()) {
			launched_fps_ = fps; // -fps sizes the helper's per-display queues
			supervisor_->restart(launch_config());
		}
		// Minor 4: nothing reads the frames while the helper is missing or its
		// supervisor gave up; the display status line already says why, so
		// don't spend an encoder session (e.g. NVENC) on them.
		if (helper_missing_ || (supervisor_ && supervisor_->status() == airplay::HelperStatus::Failed)) {
			output_error_.clear();
		} else {
			std::string error;
			OutputConfig config{settings_.global.audio_track, settings_.global.preset,
					    settings_.global.encoder_override};
			if (output_->start(config, &error)) {
				ever_started_ = true;
				output_error_.clear();
				note_activity();
				// Ruling R7: one check about 3 s after the output starts, so
				// a helper that comes up delayed on a session gets an early
				// measurement instead of waiting for the first 15 s tick.
				QTimer::singleShot(3000, this, [this] {
					if (!shut_down_)
						update_leads();
				});
			} else {
				output_error_ = error;
				log(airplay::LogLevel::Error, "AirPlay output did not start: " + error);
			}
		}
	} else if (!want && output_->active()) {
		output_->stop();
		output_stopped_since_ns_ = os_gettime_ns();
	}
	resync_displays(false);
	emit status_changed();
	emit rows_changed();
}

airplay::IdleInputs Controller::idle_inputs() const
{
	airplay::IdleInputs in;
	in.output_running = output_->active();
	in.ever_started = ever_started_;
	in.obs_busy = obs_frontend_streaming_active() || obs_frontend_recording_active();
	in.now_ns = os_gettime_ns();
	in.last_activity_ns = last_activity_ns_;
	in.output_stopped_since_ns = output_stopped_since_ns_;
	in.program_scene = program_scene_;
	return in;
}

void Controller::read_program_scene()
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	program_scene_ = scene ? obs_source_get_name(scene) : "";
	obs_source_release(scene);
}

void Controller::resync_displays(bool force)
{
	// A display in a session keeps the lead it started with: the helper uses
	// the lead from set_displays at its next (re)connect, so sending a new one
	// would change it behind applied_leads_' back. Every other display gets
	// its wanted lead for its next session.
	std::map<std::string, int> leads = wanted_leads();
	std::set<std::string> kept;
	for (auto &[id, lead] : leads) {
		const auto applied = applied_leads_.find(id);
		if (applied != applied_leads_.end() && view_->in_session(id)) {
			lead = applied->second;
			kept.insert(id);
		}
	}
	std::vector<airplay::DisplaySelection> selection = airplay::selection_for(settings_, idle_inputs(), leads);
	for (airplay::DisplaySelection &sel : selection) {
		const airplay::DisplaySettings *sd = settings_.find_display(sel.device_id);
		if (live_lead_ && sd && auto_lead(*sd))
			sel.lead_headroom_ms = sd->wifi_tolerant ? airplay::kWifiLeadHeadroomMs : airplay::kLeadHeadroomMs;
		applied_leads_[sel.device_id] = sel.latency_ms;
		if (kept.count(sel.device_id))
			continue;
		// A new session lead: remember what it was computed from, so
		// update_leads() can tell an audio-age rise from a video-need one.
		if (const airplay::DisplaySettings *d = settings_.find_display(sel.device_id)) {
			session_video_need_[sel.device_id] = d->video_need_ms;
			session_late_floor_[sel.device_id] = d->late_floor_ms;
		}
	}
	if (!force && displays_sent_ && selection == last_sent_)
		return;
	supervisor_->send_command(airplay::set_displays_command(selection));
	last_sent_ = std::move(selection);
	displays_sent_ = true;
}

bool Controller::save_settings()
{
	// Learned values stay within what settings_from_json accepts back.
	for (airplay::DisplaySettings &d : settings_.displays) {
		d.video_need_ms = std::clamp(d.video_need_ms, 0, airplay::kLeadCeilingMs);
		d.late_floor_ms = std::clamp(d.late_floor_ms, 0, airplay::kLeadCeilingMs);
	}
	std::string error;
	if (!airplay::save_settings_file(paths_.settings_file, settings_, &error)) {
		log(airplay::LogLevel::Error, "could not save settings: " + error);
		return false;
	}
	// settings.json no longer names these blobs; their OS-store items can go.
	if (view_)
		for (const std::string &blob : view_->take_pending_discards())
			pending_discards_.push_back(blob);
	for (const std::string &blob : pending_discards_)
		secrets_->discard(blob);
	pending_discards_.clear();
	return true;
}

void Controller::note_activity()
{
	last_activity_ns_ = os_gettime_ns();
}

void Controller::log(airplay::LogLevel level, const std::string &message)
{
	// Wall-clock time, not os_gettime_ns()'s monotonic encoder clock: the
	// diagnostics report renders this as a calendar date and time.
	const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
				     std::chrono::system_clock::now().time_since_epoch())
				     .count();
	log_ring_.push(static_cast<uint64_t>(wall_ns), message);
	obs_log(obs_level(level), "%s", message.c_str());
}

std::vector<airplay::DisplayRow> Controller::rows() const
{
	std::vector<airplay::DisplayRow> rows = view_->rows();
	if (output_->active())
		airplay::mark_scene_skips(rows, settings_, program_scene_);
	mark_ceiling_warnings(rows);
	return rows;
}

std::vector<airplay::DisplayGroup> Controller::groups() const
{
	std::vector<airplay::DisplayGroup> groups = view_->groups();
	if (output_->active())
		airplay::mark_scene_skips(groups, settings_, program_scene_);
	for (airplay::DisplayGroup &g : groups)
		mark_ceiling_warnings(g.displays);
	return groups;
}

void Controller::mark_ceiling_warnings(std::vector<airplay::DisplayRow> &rows) const
{
	for (airplay::DisplayRow &row : rows) {
		const auto it = ceiling_guards_.find(row.device_id);
		const airplay::DisplaySettings *d = settings_.find_display(row.device_id);
		if (!d || it == ceiling_guards_.end() || !it->second.warning() || row.light != airplay::Light::Green)
			continue;
		row.light = airplay::Light::Yellow;
		row.status_tooltip = airplay::ceiling_warning_text(d->display_name, it->second, !auto_lead(*d));
	}
}

std::vector<std::string> Controller::locations() const
{
	return view_->locations();
}

const airplay::Settings &Controller::settings() const
{
	return settings_;
}

bool Controller::running() const
{
	return output_->active();
}

void Controller::set_install_warning(const std::string &warning)
{
	install_warning_ = warning;
	log(airplay::LogLevel::Error, warning);
	emit status_changed();
}

std::string Controller::status_text() const
{
	std::string text = install_warning_.empty() ? std::string() : "Warning: " + install_warning_ + "\n";
	for (const std::string &warning : ceiling_warnings())
		text += "Warning: " + warning + "\n";
	text += output_->active() ? "Live (" + output_->encoder_id() + ")" : std::string("Stopped");
	if (!output_error_.empty())
		text += ". Output error: " + output_error_;
	text += "\nHelper: " + helper_status_;
	const auto wanted = wanted_leads();
	for (const airplay::DisplaySettings &d : settings_.displays) {
		if (!d.enabled)
			continue;
		const auto applied = applied_leads_.find(d.device_id);
		const int lead = applied != applied_leads_.end() ? applied->second : wanted.at(d.device_id);
		const bool fixed = d.latency_ms > 0 || settings_.global.target_latency_ms > 0;
		const auto live = live_leads_.find(d.device_id);
		if (!fixed && live != live_leads_.end()) {
			const airplay::LiveLead &ll = live->second;
			const auto eff_it = live_effective_.find(d.device_id);
			const int eff = eff_it != live_effective_.end() ? eff_it->second : ll.target_ms();
			text += "\n" + d.display_name + ": TV delay " + std::to_string(eff) + " ms";
			if (eff != ll.target_ms())
				text += " -> " + std::to_string(ll.target_ms());
			if (ll.at_ceiling())
				text += " (at ceiling)";
			else
				text += std::string(" (Auto ") + (ll.dynamic() ? "dynamic" : "raise only") +
					(eff != ll.target_ms() ? ", ceiling " + std::to_string(ll.ceiling_ms()) : std::string()) + ")";
			text += d.wifi_tolerant ? ", Wi-Fi" : "";
		} else {
			text += "\n" + d.display_name + ": TV delay " + std::to_string(lead) + " ms" +
				(fixed ? " (fixed)" : " (auto)") + (d.wifi_tolerant ? ", Wi-Fi" : "");
		}
		const auto loss = audio_loss_.find(d.device_id);
		if (loss != audio_loss_.end() && loss->second.seen())
			text += "; " + airplay::audio_loss_text(loss->second, os_gettime_ns());
	}
	return text;
}

std::vector<std::string> Controller::available_encoders() const
{
	return AirPlayOutput::available_encoders();
}

std::vector<std::string> Controller::scene_names() const
{
	std::vector<std::string> names;
	char **list = obs_frontend_get_scene_names();
	for (char **name = list; name && *name; ++name)
		names.emplace_back(*name);
	bfree(list);
	return names;
}

bool Controller::can_remember_passwords() const
{
	return secrets_->available();
}

std::string Controller::diagnostics_report() const
{
	std::ostringstream out;
	const std::time_t now = std::time(nullptr);
	char time_buf[64] = {};
	std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

	out << "Overflow diagnostics report\n";
	out << "Plugin version: " << PLUGIN_VERSION << "\n";
	out << "Generated: " << time_buf << "\n\n";

	out << "Helper status: " << helper_status_ << "\n";
	out << "Output: " << (output_->active() ? "active (encoder " + output_->encoder_id() + ")" : std::string("stopped"));
	if (!output_error_.empty())
		out << ", error: " << output_error_;
	out << "\n\n";

	const airplay::GlobalSettings &g = settings_.global;
	out << "Global settings:\n";
	out << "  audio_track: " << g.audio_track << "\n";
	out << "  preset: " << airplay::to_string(g.preset) << "\n";
	out << "  encoder_override: " << (g.encoder_override.empty() ? "(automatic)" : g.encoder_override) << "\n";
	out << "  start_on_launch: " << (g.start_on_launch ? "true" : "false") << "\n";
	out << "  start_with_streaming: " << (g.start_with_streaming ? "true" : "false") << "\n";
	out << "  start_with_recording: " << (g.start_with_recording ? "true" : "false") << "\n";
	out << "  target_latency_ms: " << g.target_latency_ms << "\n";
	out << "  last measured OBS audio age: " << (output_->audio_age_ns() / 1000000ull) << " ms\n";
	out << "  timing: " << airplay::to_string(g.timing) << "\n";
	out << "  eld_encoder: " << (g.eld_encoder ? "true" : "false") << "\n";
	out << "  verbose_helper_log: " << (g.verbose_helper_log ? "true" : "false") << "\n\n";

	out << "Displays:\n";
	for (const airplay::DisplaySettings &d : settings_.displays) {
		out << "  - id: " << d.device_id << "\n";
		out << "    name: " << d.display_name << "\n";
		out << "    location: " << (d.location.empty() ? "(none)" : d.location) << "\n";
		out << "    enabled: " << (d.enabled ? "true" : "false") << "\n";
		const auto applied = applied_leads_.find(d.device_id);
		out << "    latency_ms: " << d.latency_ms << (d.latency_ms > 0 ? "" : " (auto)") << "\n";
		out << "    video_need_ms: " << d.video_need_ms << "\n";
		out << "    last_run_p99_ms: " << d.last_run_p99_ms << "\n";
		out << "    late_floor_ms: " << d.late_floor_ms << "\n";
		out << "    lead_mode: " << (d.lead_mode.empty() ? "raise only" : d.lead_mode) << "\n";
		out << "    saved_lead_ms: " << d.saved_lead_ms << "\n";
		out << "    last_run_late_windows: " << d.last_run_late_windows << "\n";
		out << "    applied TV delay: "
		    << (applied != applied_leads_.end() ? std::to_string(applied->second) + " ms" : std::string("(none sent)"))
		    << "\n";
		std::string state = "(no live state)";
		std::string error_or_audio;
		for (const airplay::DisplayRow &row : view_->rows()) {
			if (row.device_id != d.device_id)
				continue;
			state = row.state.empty() ? (row.discovered ? "available" : "not found") : row.state;
			if (!row.status_tooltip.empty())
				error_or_audio = row.status_tooltip;
			if (!row.audio_reason.empty())
				error_or_audio += (error_or_audio.empty() ? "" : "; ") + row.audio_reason;
			break;
		}
		out << "    state: " << state << "\n";
		out << "    error/audio_reason: " << (error_or_audio.empty() ? "(none)" : error_or_audio) << "\n";
		// Never write the password itself: only whether one is saved.
		out << "    password: " << (d.password_protected.empty() ? "none" : "saved") << "\n";
	}
	out << "\n";

	out << "Recent log lines:\n";
	for (const airplay::LogLine &line : log_ring_.lines()) {
		char stamp[32] = {};
		const std::time_t sec = static_cast<std::time_t>(line.timestamp_ns / 1000000000ull);
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&sec));
		// Defense in depth: the helper's own debug logging fingerprints key
		// material before it ever reaches this ring (see
		// helper/internal/airplay/debug.go), but a diagnostic report is what
		// an operator hands to someone else, so scrub anything secret-shaped
		// here too. The ring itself is left untouched — only the copy that
		// goes into the report is redacted.
		out << "  [" << stamp << "] " << airplay::redact_secrets(line.text) << "\n";
	}
	return out.str();
}

void Controller::set_display_enabled(const std::string &device_id, bool enabled)
{
	settings_.ensure_display(device_id, view_->display_name_for(device_id)).enabled = enabled;
	save_settings();
	resync_displays(false);
	emit rows_changed();
}

void Controller::set_location_enabled(const std::string &location, bool enabled)
{
	for (airplay::DisplaySettings &d : settings_.displays)
		if (!location.empty() && d.location == location)
			d.enabled = enabled;
	save_settings();
	resync_displays(false);
	emit rows_changed();
}

void Controller::rename_display(const std::string &device_id, const std::string &name)
{
	settings_.ensure_display(device_id, name).display_name = name;
	save_settings();
	emit rows_changed();
}

void Controller::update_display(const airplay::DisplaySettings &display)
{
	airplay::DisplaySettings &target = settings_.ensure_display(display.device_id, display.display_name);
	const airplay::DisplaySettings before = target;
	// Merge only the fields the Display settings dialog edits. A wholesale
	// `target = display` would also overwrite password_protected with the
	// dialog's opening snapshot, discarding a password remembered (via the
	// credential prompt) while the modal dialog was still open; clearing the
	// saved password is a separate, explicit action (forget_password). It also
	// leaves device_id (already-normalized uppercase, settings.hpp) and enabled
	// (owned by the checkbox/select-all path) untouched.
	airplay::merge_dialog_fields(target, display);
	// A live change applies now; set_displays below also stores it for later sessions.
	// Receiver volume must never change unless configured: only when volume_db
	// is set and it actually changed, and never for a display with audio off
	// (R8: a video-only display's volume is never touched).
	if (target.audio_enabled && target.volume_db && target.volume_db != before.volume_db)
		supervisor_->send_command(airplay::set_volume_command(target.device_id, *target.volume_db));
	save_settings();
	const bool address_changed = target.manual_ip != before.manual_ip || target.manual_port != before.manual_port;
	const bool audio_changed = target.audio_enabled != before.audio_enabled ||
				   target.audio_format != before.audio_format;
	const bool wifi_changed = target.wifi_tolerant != before.wifi_tolerant;
	const bool latency_changed = target.latency_ms != before.latency_ms || target.lead_mode != before.lead_mode;
	const bool restart = (address_changed || audio_changed || wifi_changed || latency_changed) && target.enabled;
	// A restart starts a new session: let it take the lead these settings want
	// (resync_displays otherwise keeps a live display's current one).
	if (restart) {
		applied_leads_.erase(target.device_id);
		live_leads_.erase(target.device_id);
		at_floor_logged_.erase(target.device_id);
		live_effective_.erase(target.device_id);
	}
	resync_displays(false);
	if (restart)
		supervisor_->send_command(airplay::restart_command(target.device_id));
	emit rows_changed();
}

void Controller::update_global(const airplay::GlobalSettings &global)
{
	const airplay::GlobalSettings before = settings_.global;
	settings_.global = global;
	autostart_.set_options(airplay::autostart_options(global));
	save_settings();

	const bool helper_flags_changed = global.timing != before.timing ||
					  global.target_latency_ms != before.target_latency_ms ||
					  global.eld_encoder != before.eld_encoder ||
					  global.verbose_helper_log != before.verbose_helper_log;
	if (helper_flags_changed && loaded_ && helper_exe_present())
		supervisor_->restart(launch_config());

	// AirPlayOutput::start() ignores its config while the output is already
	// active (see the header comment on start()), so a live preset, encoder
	// override or audio-track change must stop() first; apply_run_state()
	// below starts it again with the new settings.
	const bool output_changed = global.audio_track != before.audio_track || global.preset != before.preset ||
				    global.encoder_override != before.encoder_override;
	if (output_changed && output_->active()) {
		output_->stop();
		output_stopped_since_ns_ = os_gettime_ns();
	}
	apply_run_state();
}

void Controller::add_display(const std::string &name, const std::string &ip, int port, const std::string &location)
{
	// R4: device IDs are uppercase everywhere; ensure_display would uppercase
	// this anyway, but write it uppercase so the intent is clear.
	const std::string id = "MANUAL-" + ip;
	airplay::DisplaySettings &display = settings_.ensure_display(id, name.empty() ? ip : name);
	display.manual_ip = ip;
	display.manual_port = port;
	display.location = location;
	display.enabled = true;
	save_settings();
	resync_displays(false);
	emit rows_changed();
}

void Controller::user_start()
{
	autostart_.user_start();
	ensure_helper(); // an explicit Start retries a missing or failed helper now
	apply_run_state();
}

void Controller::user_stop()
{
	autostart_.user_stop();
	apply_run_state();
}

void Controller::restart_display(const std::string &device_id)
{
	// A restart starts a new session: let it take the lead wanted now.
	applied_leads_.erase(to_upper_ascii(device_id));
	live_leads_.erase(to_upper_ascii(device_id));
	at_floor_logged_.erase(to_upper_ascii(device_id));
	live_effective_.erase(to_upper_ascii(device_id));
	resync_displays(false);
	view_->restart(device_id);
}

void Controller::reconnect_display(const std::string &device_id)
{
	view_->reconnect(device_id);
}

void Controller::forget_display(const std::string &device_id)
{
	// Clears the pairing and saved password only (Important-1 ruling): a
	// manual display, or a saved display that happens to be offline right
	// now, must never lose its settings entry just because Forget was used.
	view_->forget(device_id);
	save_settings();
	resync_displays(false);
	emit rows_changed();
}

void Controller::remove_display(const std::string &device_id)
{
	// Removing the entry drops it from selection_for()'s input; resync_displays
	// below picks that up and, if it was enabled, sends the smaller set.
	if (const airplay::DisplaySettings *d = settings_.find_display(device_id))
		if (!d->password_protected.empty())
			pending_discards_.push_back(d->password_protected); // its OS-store item, after the save
	settings_.remove_display(device_id);
	save_settings();
	resync_displays(false);
	emit rows_changed();
}

void Controller::forget_password(const std::string &device_id)
{
	view_->forget_password(device_id);
	if (view_->take_settings_dirty())
		save_settings();
	emit rows_changed();
}

void Controller::answer_credential(const std::string &device_id, const std::string &value, bool remember)
{
	view_->answer_credential(device_id, value, remember);
	if (view_->take_settings_dirty())
		save_settings();
}
