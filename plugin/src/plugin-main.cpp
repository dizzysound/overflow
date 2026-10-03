// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay-output.hpp"
#include "airplay/config_migration.hpp"
#include "controller.hpp"
#include "ui/airplay-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QObject>
#include <QPointer>

#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

Controller *g_controller = nullptr;
// OBS owns the dock through its dock frame; QPointer goes null if OBS
// destroys it first.
QPointer<AirPlayDock> g_dock;

// Tools > Overflow: shows and raises the dock (obs_frontend_cb).
void show_airplay_dock(void *)
{
	if (!g_dock)
		return;
	// The dock frame OBS wraps the widget in is its parent; go through it so
	// a hidden or tabbed-under dock actually comes to the front.
	if (auto *dock_widget = qobject_cast<QDockWidget *>(g_dock->parentWidget())) {
		dock_widget->setVisible(true);
		dock_widget->raise();
		if (dock_widget->isFloating())
			dock_widget->activateWindow();
		return;
	}
	g_dock->show(); // fallback: the QDockWidget lookup above failed
}

// Tools > Overflow: Save Diagnostic Log... (obs_frontend_cb, called on
// the UI thread, like show_airplay_dock above).
void save_airplay_diagnostic_log(void *)
{
	if (!g_dock)
		return;
	g_dock->save_diagnostic_log();
}

std::string module_bin_dir()
{
	const char *path = obs_get_module_binary_path(obs_current_module());
	const std::string p = path ? path : "";
	const size_t slash = p.find_last_of("/\\");
	return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::string config_path(const char *file)
{
	char *p = obs_module_config_path(file);
	const std::string s = p ? p : "";
	bfree(p);
	return s;
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (g_controller)
		g_controller->handle_frontend_event(event);
}

// The plugin's name before it became Overflow (obs-overflow).
constexpr const char *kLegacyModule = "obs-airplay";

// Copies the old plugin's settings and pairings into this plugin's folder the
// first time it runs, so the rooms survive the rename.
void migrate_legacy_config(const std::string &config_dir)
{
	const std::string old_dir = airplay::legacy_config_dir(config_dir, kLegacyModule);
	const airplay::MigrationResult r =
		airplay::migrate_legacy_config(old_dir, config_dir, {"settings.json", "credentials.json"});
	for (const std::string &file : r.copied)
		obs_log(LOG_INFO, "copied %s from %s", file.c_str(), old_dir.c_str());
	if (!r.error.empty())
		obs_log(LOG_WARNING, "settings from %s were not copied: %s", kLegacyModule, r.error.c_str());
}

} // namespace

bool obs_module_load(void)
{
	AirPlayOutput::register_output_type();

	const std::string config_dir = config_path("");
	if (!config_dir.empty()) {
		os_mkdirs(config_dir.c_str());
		migrate_legacy_config(config_dir);
	}

	const std::string bin = module_bin_dir();
	Controller::Paths paths;
#ifdef _WIN32
	paths.helper_exe = bin + "/overflow-helper.exe";
#else
	paths.helper_exe = bin + "/overflow-helper";
#endif
	paths.eld_disabled_path = bin + "/eld-encoder.disabled";
	paths.settings_file = config_path("settings.json");
	paths.creds_file = config_path("credentials.json");
	g_controller = new Controller(paths);

	// Registered at load time, before OBS restores its dock layout
	// (OBSBasic::OBSInit loads modules, then calls restoreState).
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = new AirPlayDock(g_controller, main_window);
	QObject::connect(g_controller, &Controller::rows_changed, dock, [dock] { dock->refresh(); }, Qt::QueuedConnection);
	QObject::connect(g_controller, &Controller::status_changed, dock, [dock] { dock->refresh(); }, Qt::QueuedConnection);
	QObject::connect(g_controller, &Controller::credential_requested, dock,
			 [dock](const QString &id, const QString &name, const QString &kind, bool retry) {
				 dock->prompt_credential(id, name, kind, retry);
			 });
	obs_frontend_add_dock_by_id("obs-overflow-displays", "Overflow", dock);
	g_dock = dock;
	obs_frontend_add_tools_menu_item("Overflow", show_airplay_dock, nullptr);
	obs_frontend_add_tools_menu_item("Overflow: Save Diagnostic Log...", save_airplay_diagnostic_log, nullptr);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

// Every module is loaded by now. Two copies of the plugin would both drive the
// same TVs, so say so in the dock until the old one is removed.
void obs_module_post_load(void)
{
	if (g_controller && obs_get_module(kLegacyModule))
		g_controller->set_install_warning(std::string("the old ") + kLegacyModule +
						  " plugin is still installed; remove it and restart OBS");
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	if (g_controller) {
		g_controller->shutdown();
		// The dock holds a raw DockBackend pointer and may still have queued
		// refreshes; delete it first (Qt drops its pending events) so nothing
		// reaches the controller after it is freed.
		delete g_dock.data();
		delete g_controller;
		g_controller = nullptr;
	}
	obs_log(LOG_INFO, "plugin unloaded");
}
