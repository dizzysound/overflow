// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Renders the real dock and dialogs, filled with sample rooms, to PNG files
// for the README. No OBS: a fixed DockBackend stands in for the controller,
// and a dark palette stands in for OBS's default theme.
//
//   cmake -S plugin/tests -B build-shots -DAIRPLAY_SCREENSHOTS=ON -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
//   cmake --build build-shots --target screenshots
//   build-shots/screenshots docs/images

#include "airplay-dock.hpp"
#include "display-settings-dialog.hpp"
#include "global-settings-dialog.hpp"

#include <QApplication>
#include <QDir>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>
#include <QTreeWidget>

#include <cstdio>
#include <string>
#include <vector>

namespace {

airplay::DisplayRow row(const char *id, const char *name, const char *location, const char *model, bool enabled,
			const char *state, airplay::Light light)
{
	airplay::DisplayRow r;
	r.device_id = id;
	r.display_name = name;
	r.location = location;
	r.model = model;
	r.enabled = enabled;
	r.discovered = true;
	r.state = state;
	r.light = light;
	return r;
}

class SampleBackend final : public DockBackend {
public:
	SampleBackend()
	{
		airplay::DisplayGroup overflow;
		overflow.location = "Overflow Room";
		overflow.check = airplay::GroupCheck::All;
		overflow.displays = {row("A1", "Front TV", "Overflow Room", "AppleTV5,3", true, "live", airplay::Light::Green),
				     row("A2", "Back TV", "Overflow Room", "AppleTV11,1", true, "live", airplay::Light::Green)};
		airplay::DisplayGroup hall;
		hall.location = "Fellowship Hall";
		hall.check = airplay::GroupCheck::All;
		hall.displays = {row("R1", "Hall Roku TV", "Fellowship Hall", "G218X", true, "live", airplay::Light::Green),
				 row("P1", "Hall Panel", "Fellowship Hall", "AppleTV3,2", true, "connecting", airplay::Light::Yellow)};
		airplay::DisplayGroup lobby;
		lobby.location = "Lobby";
		lobby.check = airplay::GroupCheck::None;
		lobby.displays = {row("L1", "Lobby TV", "Lobby", "AppleTV6,2", false, "", airplay::Light::Gray)};
		groups_ = {overflow, hall, lobby};

		settings_.global.start_on_launch = true;
		settings_.global.start_with_streaming = true;
		airplay::DisplaySettings &d = settings_.ensure_display("A1", "Front TV");
		d.location = "Overflow Room";
		d.enabled = true;
	}

	std::vector<airplay::DisplayRow> rows() const override
	{
		std::vector<airplay::DisplayRow> all;
		for (const auto &g : groups_)
			all.insert(all.end(), g.displays.begin(), g.displays.end());
		return all;
	}
	std::vector<airplay::DisplayGroup> groups() const override { return groups_; }
	std::vector<std::string> locations() const override { return {"Overflow Room", "Fellowship Hall", "Lobby"}; }
	const airplay::Settings &settings() const override { return settings_; }
	bool running() const override { return true; }
	std::string status_text() const override
	{
		return "Live (obs_nvenc_h264_tex)\n"
		       "Helper: running\n"
		       "Front TV: TV delay 180 ms (Auto raise only); no audio loss in the last minute\n"
		       "Back TV: TV delay 180 ms (Auto raise only); no audio loss in the last minute\n"
		       "Hall Roku TV: TV delay 225 ms (Auto raise only), Wi-Fi; no audio loss in the last minute\n"
		       "Hall Panel: TV delay 110 ms (auto)";
	}
	std::vector<std::string> available_encoders() const override
	{
		return {"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2", "obs_x264"};
	}
	bool can_remember_passwords() const override { return true; }
	std::string diagnostics_report() const override { return {}; }
	void set_display_enabled(const std::string &, bool) override {}
	void set_location_enabled(const std::string &, bool) override {}
	void rename_display(const std::string &, const std::string &) override {}
	void update_display(const airplay::DisplaySettings &) override {}
	void update_global(const airplay::GlobalSettings &) override {}
	void add_display(const std::string &, const std::string &, int, const std::string &) override {}
	void user_start() override {}
	void user_stop() override {}
	void restart_display(const std::string &) override {}
	void reconnect_display(const std::string &) override {}
	void forget_display(const std::string &) override {}
	void remove_display(const std::string &) override {}
	void forget_password(const std::string &) override {}
	void answer_credential(const std::string &, const std::string &, bool) override {}

private:
	std::vector<airplay::DisplayGroup> groups_;
	airplay::Settings settings_;
};

// Close to OBS Studio's default dark theme.
void use_dark_palette(QApplication &app)
{
	app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
	QPalette p;
	const QColor window(0x27, 0x2a, 0x33), base(0x1f, 0x21, 0x29), text(0xe6, 0xe6, 0xe6),
		button(0x3c, 0x40, 0x4d), highlight(0x47, 0x6b, 0xe6), mid(0x5a, 0x5e, 0x6b);
	p.setColor(QPalette::Window, window);
	p.setColor(QPalette::WindowText, text);
	p.setColor(QPalette::Base, base);
	p.setColor(QPalette::AlternateBase, window);
	p.setColor(QPalette::Text, text);
	p.setColor(QPalette::Button, button);
	p.setColor(QPalette::ButtonText, text);
	p.setColor(QPalette::Highlight, highlight);
	p.setColor(QPalette::HighlightedText, Qt::white);
	p.setColor(QPalette::ToolTipBase, base);
	p.setColor(QPalette::ToolTipText, text);
	p.setColor(QPalette::Mid, mid);
	p.setColor(QPalette::Disabled, QPalette::Text, mid);
	p.setColor(QPalette::Disabled, QPalette::WindowText, mid);
	p.setColor(QPalette::Disabled, QPalette::ButtonText, mid);
	app.setPalette(p);
}

bool save(QWidget &w, const QString &path)
{
	w.ensurePolished();
	w.show();
	QApplication::processEvents();
	const bool ok = w.grab().save(path);
	std::printf("%s %s (%dx%d)\n", ok ? "wrote" : "FAILED", qPrintable(path), w.width(), w.height());
	w.hide();
	return ok;
}

} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	use_dark_palette(app);
	const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
	QDir().mkpath(out);
	bool ok = true;

	SampleBackend backend;
	AirPlayDock dock(&backend);
	dock.resize(560, 430);
	dock.refresh();
	if (auto *tree = dock.findChild<QTreeWidget *>()) {
		tree->expandAll();
		// Select the first display so its settings button is enabled.
		const QList<QTreeWidgetItem *> hits = tree->findItems(QStringLiteral("Front TV"), Qt::MatchRecursive);
		if (!hits.isEmpty())
			tree->setCurrentItem(hits.front());
	}
	ok &= save(dock, out + QStringLiteral("/dock.png"));

	airplay::DisplaySettings display = backend.settings().displays.front();
	display.wifi_tolerant = false;
	display.volume_db.reset();
	DisplaySettingsDialog display_dialog(display, backend.locations());
	display_dialog.adjustSize();
	ok &= save(display_dialog, out + QStringLiteral("/display-settings.png"));

	GlobalSettingsDialog global_dialog(backend.settings().global, backend.available_encoders());
	global_dialog.adjustSize();
	ok &= save(global_dialog, out + QStringLiteral("/settings.png"));

	return ok ? 0 : 1;
}
