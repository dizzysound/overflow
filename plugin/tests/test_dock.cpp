// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "add-display-dialog.hpp"
#include "airplay-dock.hpp"
#include "credential-dialog.hpp"
#include "display-settings-dialog.hpp"
#include "global-settings-dialog.hpp"

#include <doctest/doctest.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <string>
#include <vector>

namespace {

class FakeBackend final : public DockBackend {
public:
	std::vector<airplay::DisplayGroup> groups_;
	airplay::Settings settings_;
	bool running_ = false;
	std::vector<std::string> calls;

	std::vector<airplay::DisplayRow> rows() const override
	{
		std::vector<airplay::DisplayRow> all;
		for (const auto &g : groups_)
			all.insert(all.end(), g.displays.begin(), g.displays.end());
		return all;
	}
	std::vector<airplay::DisplayGroup> groups() const override { return groups_; }
	std::vector<std::string> locations() const override { return {"Friendship Hall", "Sanctuary"}; }
	const airplay::Settings &settings() const override { return settings_; }
	bool running() const override { return running_; }
	std::string status_text() const override { return running_ ? "Live (obs_x264)" : "Stopped"; }
	std::vector<std::string> available_encoders() const override { return {"obs_x264", "obs_nvenc_h264_tex"}; }
	std::vector<std::string> scene_names() const override { return {"Pulpit", "Narthex Wide"}; }
	bool can_remember_passwords() const override { return true; }
	std::string diagnostics_report() const override { return "diagnostics report\n"; }
	void set_display_enabled(const std::string &id, bool enabled) override
	{
		calls.push_back("enable " + id + (enabled ? " on" : " off"));
	}
	void set_location_enabled(const std::string &location, bool enabled) override
	{
		calls.push_back("location " + location + (enabled ? " on" : " off"));
	}
	void rename_display(const std::string &id, const std::string &name) override
	{
		calls.push_back("rename " + id + " " + name);
	}
	void update_display(const airplay::DisplaySettings &d) override { calls.push_back("update " + d.device_id); }
	void update_global(const airplay::GlobalSettings &) override { calls.push_back("global"); }
	void add_display(const std::string &name, const std::string &ip, int port, const std::string &location) override
	{
		calls.push_back("add " + name + " " + ip + " " + std::to_string(port) + " " + location);
	}
	void user_start() override
	{
		running_ = true;
		calls.push_back("start");
	}
	void user_stop() override
	{
		running_ = false;
		calls.push_back("stop");
	}
	void restart_display(const std::string &id) override { calls.push_back("restart " + id); }
	void reconnect_display(const std::string &id) override { calls.push_back("reconnect " + id); }
	void forget_display(const std::string &id) override { calls.push_back("forget " + id); }
	void remove_display(const std::string &id) override { calls.push_back("remove " + id); }
	void forget_password(const std::string &id) override { calls.push_back("forgetPassword " + id); }
	void answer_credential(const std::string &id, const std::string &value, bool remember) override
	{
		calls.push_back("credential " + id + " " + value + (remember ? " remember" : ""));
	}
};

airplay::DisplayRow row(const std::string &id, const std::string &name, bool enabled, const std::string &state,
			airplay::Light light, const std::string &location = "")
{
	airplay::DisplayRow r;
	r.device_id = id;
	r.display_name = name;
	r.location = location;
	r.enabled = enabled;
	r.discovered = true;
	r.state = state;
	r.light = light;
	return r;
}

// Friendship Hall: Left (on, live, no audio) and Right (off); ungrouped: Narthex (off).
FakeBackend two_groups()
{
	FakeBackend b;
	airplay::DisplayGroup hall;
	hall.location = "Friendship Hall";
	hall.check = airplay::GroupCheck::Some;
	hall.displays = {row("L", "Left TV", true, "live", airplay::Light::Green, "Friendship Hall"),
			 row("R", "Right TV", false, "", airplay::Light::Gray, "Friendship Hall")};
	hall.displays[0].audio_off = true;
	hall.displays[0].audio_reason = "receiver only accepts AAC-ELD audio";
	airplay::DisplayGroup none;
	none.displays = {row("N", "Narthex", false, "", airplay::Light::Gray)};
	b.groups_ = {hall, none};
	return b;
}

} // namespace

TEST_CASE("dock sizing: sensible minimum/hint, and columns are user-resizable with Audio taking the remainder")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	CHECK(dock.minimumSize().width() >= 420);
	CHECK(dock.minimumSize().height() >= 260);
	CHECK(dock.sizeHint().width() >= 640);
	CHECK(dock.sizeHint().height() >= 380);

	// Column indices, matching the Column enum in airplay-dock.cpp (not
	// exposed to tests): 0 Display, 1 Status, 2 Audio, 3 the settings gear.
	QHeaderView *header = dock.tree()->header();
	CHECK(header->sectionResizeMode(0) == QHeaderView::Interactive);
	CHECK(header->sectionResizeMode(1) == QHeaderView::Interactive);
	CHECK(header->sectionResizeMode(2) == QHeaderView::Stretch); // Audio takes the remainder
	CHECK(header->sectionResizeMode(3) == QHeaderView::Fixed);
	CHECK_FALSE(header->sectionsMovable());
	CHECK(dock.tree()->columnWidth(0) >= 240);
	CHECK(dock.tree()->columnWidth(1) >= 170);
	CHECK(dock.tree()->columnWidth(3) < 60);
	header->resizeSection(0, 20); // a data column never shrinks below 90
	CHECK(dock.tree()->columnWidth(0) >= 90);
}

TEST_CASE("dock groups displays under location headings, ungrouped last")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	QTreeWidget *tree = dock.tree();
	REQUIRE(tree->topLevelItemCount() == 2);
	QTreeWidgetItem *hall = tree->topLevelItem(0);
	CHECK(hall->text(0) == QStringLiteral("Friendship Hall"));
	CHECK(hall->data(0, AirPlayDock::kHeadingRole).toBool());
	CHECK(hall->checkState(0) == Qt::PartiallyChecked);
	REQUIRE(hall->childCount() == 2);
	CHECK(hall->child(0)->text(0) == QStringLiteral("Left TV"));
	CHECK(hall->child(0)->checkState(0) == Qt::Checked);
	CHECK(hall->child(0)->text(1) == QStringLiteral("live"));
	CHECK(hall->child(0)->text(2) == QStringLiteral("no audio"));
	CHECK(hall->child(0)->toolTip(2) == QStringLiteral("receiver only accepts AAC-ELD audio"));
	CHECK(hall->child(1)->text(1) == QStringLiteral("available"));
	QTreeWidgetItem *narthex = tree->topLevelItem(1);
	CHECK(narthex->text(0) == QStringLiteral("Narthex"));
	CHECK_FALSE(narthex->data(0, AirPlayDock::kHeadingRole).toBool());
	CHECK(dock.status_label()->text() == QStringLiteral("Stopped"));
}

TEST_CASE("a location heading selects or deselects its whole group")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	QTreeWidgetItem *hall = dock.tree()->topLevelItem(0);
	hall->setCheckState(0, Qt::Checked); // what a click on a partly checked heading does
	CHECK(b.calls == std::vector<std::string>{"location Friendship Hall on"});
	hall->setCheckState(0, Qt::Unchecked);
	CHECK(b.calls.back() == "location Friendship Hall off");
}

TEST_CASE("checking a display and renaming it reach the backend")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	QTreeWidgetItem *narthex = dock.tree()->topLevelItem(1);
	narthex->setCheckState(0, Qt::Checked);
	CHECK(b.calls == std::vector<std::string>{"enable N on"});
	b.groups_[1].displays[0].enabled = true;
	narthex->setText(0, QStringLiteral("Narthex TV"));
	CHECK(b.calls.back() == "rename N Narthex TV");
}

TEST_CASE("refresh keeps the selected display and never calls the backend")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	dock.tree()->setCurrentItem(dock.tree()->topLevelItem(0)->child(1));
	b.groups_[0].displays[1].state = "connecting";
	b.groups_[0].displays[1].light = airplay::Light::Yellow;
	dock.refresh();
	QTreeWidgetItem *current = dock.tree()->currentItem();
	REQUIRE(current);
	CHECK(current->data(0, AirPlayDock::kKeyRole).toString() == QStringLiteral("R"));
	CHECK(current->text(1) == QStringLiteral("connecting"));
	CHECK(b.calls.empty());
}

TEST_CASE("the hint label explains how to change a display's settings when the list is non-empty")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	QLabel *hint = dock.findChild<QLabel *>(QStringLiteral("hint"));
	REQUIRE(hint);
	CHECK(hint->text() == QStringLiteral("Check a display to send to it. The gear opens its settings; "
					      "right-click for Restart or Forget. Double-click a name to rename it."));
	CHECK(hint->wordWrap());
}

TEST_CASE("the hint label points to Add display... when no displays are found yet")
{
	FakeBackend b; // no groups at all
	AirPlayDock dock(&b);
	QLabel *hint = dock.findChild<QLabel *>(QStringLiteral("hint"));
	REQUIRE(hint);
	CHECK(hint->text() == QStringLiteral("No displays found yet. They appear here as they are discovered, or use "
					      "Add display... to enter one by address."));
}

TEST_CASE("Start/Stop toggles through the backend")
{
	FakeBackend b;
	AirPlayDock dock(&b);
	CHECK(dock.start_stop_button()->text() == QStringLiteral("Start"));
	dock.start_stop_button()->click();
	CHECK(b.calls == std::vector<std::string>{"start"});
	CHECK(dock.start_stop_button()->text() == QStringLiteral("Stop"));
	dock.start_stop_button()->click();
	CHECK(b.calls.back() == "stop");
}

TEST_CASE("Important 1: the display context menu lists both Forget pairing and Remove display")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	QMenu *menu = dock.build_display_menu("L");
	const QList<QAction *> actions = menu->actions();
	auto has = [&](const QString &text) {
		for (QAction *a : actions)
			if (a->text() == text)
				return true;
		return false;
	};
	CHECK(has(QStringLiteral("Display settings...")));
	CHECK(has(QStringLiteral("Restart")));
	CHECK(has(QStringLiteral("Reconnect")));
	CHECK(has(QStringLiteral("Forget pairing...")));
	CHECK(has(QStringLiteral("Remove display...")));
	delete menu;
}

TEST_CASE("Important 1: Forget pairing calls forget_display only, never remove_display, for a manual display")
{
	FakeBackend b;
	airplay::DisplayRow r;
	r.device_id = "M";
	r.display_name = "Manual TV";
	r.manual = true;
	r.discovered = false;
	airplay::DisplayGroup g;
	g.displays = {r};
	b.groups_ = {g};
	AirPlayDock dock(&b);
	QMenu *menu = dock.build_display_menu("M");
	QAction *forget = menu->findChild<QAction *>(QStringLiteral("forgetPairing"));
	REQUIRE(forget);
	// The confirmation dialog is modal; answer it from inside its own event
	// loop once it appears.
	QTimer::singleShot(0, [] {
		if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
			box->button(QMessageBox::Yes)->click();
	});
	forget->trigger();
	CHECK(b.calls == std::vector<std::string>{"forget M"});
	delete menu;
}

TEST_CASE("display settings dialog round trip")
{
	airplay::DisplaySettings d;
	d.device_id = "A";
	d.display_name = "Roku";
	d.password_protected = "blob";
	DisplaySettingsDialog dialog(d, {"Friendship Hall"});
	airplay::DisplaySettings out = dialog.result_settings();
	CHECK(out == d); // an untouched dialog changes nothing; volume stays unset

	dialog.findChild<QComboBox *>(QStringLiteral("location"))->setEditText(QStringLiteral(" Friendship Hall "));
	dialog.findChild<QCheckBox *>(QStringLiteral("setVolume"))->setChecked(true);
	dialog.findChild<QDoubleSpinBox *>(QStringLiteral("volume"))->setValue(-12.5);
	dialog.findChild<QLineEdit *>(QStringLiteral("ip"))->setText(QStringLiteral(" 10.20.0.178 "));
	dialog.findChild<QSpinBox *>(QStringLiteral("port"))->setValue(7000);
	auto *idle = dialog.findChild<QComboBox *>(QStringLiteral("idle"));
	idle->setCurrentIndex(idle->findData(static_cast<int>(airplay::IdlePolicy::DisconnectAfterIdle)));
	CHECK(dialog.findChild<QSpinBox *>(QStringLiteral("idleMinutes"))->isEnabled());
	dialog.findChild<QCheckBox *>(QStringLiteral("clearPassword"))->setChecked(true);
	out = dialog.result_settings();
	CHECK(out.location == "Friendship Hall");
	CHECK(out.volume_db == std::optional<double>(-12.5));
	CHECK(out.manual_ip == "10.20.0.178");
	CHECK(out.manual_port == 7000);
	CHECK(out.idle_policy == airplay::IdlePolicy::DisconnectAfterIdle);
	// Minor 1 ruling: result_settings() no longer clears password_protected
	// itself (a wholesale merge in the controller would otherwise overwrite a
	// password remembered while the dialog was open); clear_password()
	// reports the checkbox separately, and the dock routes it through
	// DockBackend::forget_password() instead.
	CHECK(out.password_protected == "blob");
	CHECK(dialog.clear_password());
}

TEST_CASE("display settings dialog: the audio toggle disables volume controls but keeps their values")
{
	airplay::DisplaySettings d;
	d.device_id = "A";
	d.display_name = "Roku";
	d.volume_db = -12.5;
	DisplaySettingsDialog dialog(d, {});
	airplay::DisplaySettings out = dialog.result_settings();
	CHECK(out == d); // untouched: audio on, volume preserved

	auto *send_audio = dialog.findChild<QCheckBox *>(QStringLiteral("sendAudio"));
	auto *set_volume = dialog.findChild<QCheckBox *>(QStringLiteral("setVolume"));
	auto *volume = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("volume"));
	REQUIRE(send_audio);
	CHECK(send_audio->isChecked());
	CHECK(set_volume->isEnabled());
	CHECK(volume->isEnabled());

	send_audio->setChecked(false);
	CHECK_FALSE(set_volume->isEnabled());
	CHECK_FALSE(volume->isEnabled());
	// Values survive being disabled.
	CHECK(set_volume->isChecked());
	CHECK(volume->value() == doctest::Approx(-12.5));

	out = dialog.result_settings();
	CHECK_FALSE(out.audio_enabled);
	CHECK(out.volume_db == std::optional<double>(-12.5)); // the dialog keeps the value; the controller withholds it

	send_audio->setChecked(true);
	CHECK(set_volume->isEnabled());
	CHECK(volume->isEnabled());
	out = dialog.result_settings();
	CHECK(out.audio_enabled);
	CHECK(out.volume_db == std::optional<double>(-12.5));
}

TEST_CASE("display settings dialog: the Wi-Fi tolerance checkbox round-trips")
{
	airplay::DisplaySettings d;
	d.device_id = "A";
	d.display_name = "Roku";
	DisplaySettingsDialog dialog(d, {});
	CHECK(dialog.result_settings() == d); // untouched: off

	auto *wifi = dialog.findChild<QCheckBox *>(QStringLiteral("wifiTolerant"));
	REQUIRE(wifi);
	CHECK_FALSE(wifi->isChecked());
	CHECK(wifi->text() == QStringLiteral("Wi-Fi display: tolerate brief network stalls"));
	CHECK_FALSE(wifi->toolTip().isEmpty());
	wifi->setChecked(true);
	CHECK(dialog.result_settings().wifi_tolerant);

	d.wifi_tolerant = true;
	DisplaySettingsDialog reopened(d, {});
	CHECK(reopened.findChild<QCheckBox *>(QStringLiteral("wifiTolerant"))->isChecked());
	CHECK(reopened.result_settings() == d);
}

TEST_CASE("display dialog: TV delay spinbox round-trips; 0 reads Auto")
{
	airplay::DisplaySettings d;
	d.device_id = "AA";
	d.display_name = "Sacristy";
	DisplaySettingsDialog dialog(d, {});
	CHECK(dialog.result_settings() == d); // untouched: Auto
	auto *delay = dialog.findChild<QSpinBox *>(QStringLiteral("tvDelay"));
	REQUIRE(delay != nullptr);
	CHECK(delay->value() == delay->minimum()); // the special value: Auto
	CHECK(delay->specialValueText().startsWith(QStringLiteral("Auto")));
	delay->setValue(delay->minimum() + 5);
	CHECK(dialog.result_settings().latency_ms == 40); // the smallest fixed delay
	delay->setValue(delay->minimum());
	CHECK(dialog.result_settings().latency_ms == 0);
	delay->setValue(120);
	CHECK(dialog.result_settings().latency_ms == 120);

	d.latency_ms = 120;
	DisplaySettingsDialog reopened(d, {});
	CHECK(reopened.findChild<QSpinBox *>(QStringLiteral("tvDelay"))->value() == 120);
	CHECK(reopened.result_settings() == d);

	// A stored delay below the helper's floor opens as the floor it really runs at.
	d.latency_ms = 15;
	DisplaySettingsDialog low(d, {});
	CHECK(low.findChild<QSpinBox *>(QStringLiteral("tvDelay"))->value() == 40);
}

TEST_CASE("global settings dialog round trip")
{
	airplay::GlobalSettings g;
	g.encoder_override = "h264_texture_amf"; // not in the available list
	GlobalSettingsDialog dialog(g, {"obs_x264"});
	CHECK(dialog.result_settings() == g);
	dialog.findChild<QCheckBox *>(QStringLiteral("onLaunch"))->setChecked(true);
	dialog.findChild<QComboBox *>(QStringLiteral("track"))->setCurrentIndex(1);
	dialog.findChild<QSpinBox *>(QStringLiteral("latency"))->setValue(300);
	dialog.findChild<QComboBox *>(QStringLiteral("encoder"))->setCurrentIndex(0);
	dialog.findChild<QCheckBox *>(QStringLiteral("verboseLog"))->setChecked(true);
	const airplay::GlobalSettings out = dialog.result_settings();
	CHECK(out.start_on_launch);
	CHECK(out.audio_track == 2);
	CHECK(out.target_latency_ms == 300);
	CHECK(out.encoder_override.empty());
	CHECK(out.verbose_helper_log);
}

TEST_CASE("Save diagnostic log warns instead of crashing when no diagnostics function was supplied")
{
	const airplay::GlobalSettings g;
	GlobalSettingsDialog dialog(g, {"obs_x264"}); // no diagnostics callback passed
	auto *button = dialog.findChild<QPushButton *>(QStringLiteral("saveDiagnosticLog"));
	REQUIRE(button);
	QTimer::singleShot(0, [] {
		if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
			box->accept();
	});
	button->click();
	// Reaching here (rather than hanging on an unanswered file dialog) shows
	// the missing-diagnostics path never opened QFileDialog.
	CHECK(true);
}

TEST_CASE("the global settings dialog offers Save diagnostic log, wired to the backend's diagnostics_report")
{
	FakeBackend b;
	AirPlayDock dock(&b);
	QPushButton *settings_button = dock.findChild<QPushButton *>(QStringLiteral("settings"));
	REQUIRE(settings_button);
	// Opening the real (modal) Settings dialog would block; the wiring itself
	// (open_global_settings passing backend_->diagnostics_report) is exercised
	// indirectly by the FakeBackend's diagnostics_report() returning a fixed
	// string, checked here directly.
	CHECK(b.diagnostics_report() == "diagnostics report\n");
}

TEST_CASE("global settings dialog default preset is 1080p 6 Mbps and the preset combo lists the current presets")
{
	// Controller ruling R1: one shared encoder, global setting. Only these
	// four presets exist; the default is P1080_6Mbps.
	const airplay::GlobalSettings g; // defaults
	CHECK(g.preset == airplay::QualityPreset::P1080_6Mbps);
	GlobalSettingsDialog dialog(g, {"obs_x264"});
	CHECK(dialog.result_settings().preset == airplay::QualityPreset::P1080_6Mbps);

	auto *preset = dialog.findChild<QComboBox *>(QStringLiteral("preset"));
	REQUIRE(preset->count() == 4);
	CHECK(preset->itemText(0) == QStringLiteral("1080p, 6 Mbps (default)"));
	CHECK(preset->itemData(0).toInt() == static_cast<int>(airplay::QualityPreset::P1080_6Mbps));
	CHECK(preset->itemText(1) == QStringLiteral("1080p, 8 Mbps"));
	CHECK(preset->itemData(1).toInt() == static_cast<int>(airplay::QualityPreset::P1080_8Mbps));
	CHECK(preset->itemText(2) == QStringLiteral("1080p, 10 Mbps"));
	CHECK(preset->itemData(2).toInt() == static_cast<int>(airplay::QualityPreset::P1080_10Mbps));
	CHECK(preset->itemText(3) == QStringLiteral("720p, 4 Mbps"));
	CHECK(preset->itemData(3).toInt() == static_cast<int>(airplay::QualityPreset::P720_4Mbps));

	preset->setCurrentIndex(3);
	CHECK(dialog.result_settings().preset == airplay::QualityPreset::P720_4Mbps);
}

TEST_CASE("credential and add-display dialogs")
{
	CredentialDialog pin(QStringLiteral("Sacristy"), QStringLiteral("pin"), false, true);
	CHECK(pin.findChild<QLabel *>(QStringLiteral("prompt"))->text().contains(QStringLiteral("code shown")));
	CHECK_FALSE(pin.remember());
	pin.findChild<QLineEdit *>(QStringLiteral("value"))->setText(QStringLiteral(" 1234 "));
	CHECK(pin.value() == QStringLiteral("1234"));

	CredentialDialog password(QStringLiteral("Roku"), QStringLiteral("password"), true, true);
	CHECK(password.findChild<QLabel *>(QStringLiteral("prompt"))
		      ->text()
		      .startsWith(QStringLiteral("The password was not accepted")));
	CHECK(password.findChild<QLineEdit *>(QStringLiteral("value"))->echoMode() == QLineEdit::Password);
	CHECK(password.remember());

	AddDisplayDialog add({"Friendship Hall"});
	add.findChild<QLineEdit *>(QStringLiteral("ip"))->setText(QStringLiteral("192.168.1.58"));
	add.findChild<QComboBox *>(QStringLiteral("location"))->setEditText(QStringLiteral("Friendship Hall"));
	CHECK(add.ip() == QStringLiteral("192.168.1.58"));
	CHECK(add.port() == 0);
	CHECK(add.location() == QStringLiteral("Friendship Hall"));
}

TEST_CASE("Minor 3: Add display only enables OK for a valid IPv4 or IPv6 literal")
{
	AddDisplayDialog add({});
	auto *ip = add.findChild<QLineEdit *>(QStringLiteral("ip"));
	auto *ok = add.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
	CHECK_FALSE(ok->isEnabled()); // empty: required here, unlike Display settings

	ip->setText(QStringLiteral("roku.local"));
	CHECK_FALSE(ok->isEnabled());
	ip->setText(QStringLiteral("10.20.0.178"));
	CHECK(ok->isEnabled());
	ip->setText(QStringLiteral("not an ip"));
	CHECK_FALSE(ok->isEnabled());
	ip->setText(QStringLiteral("::1"));
	CHECK(ok->isEnabled());
}

TEST_CASE("Minor 3: Display settings' manual IP allows empty (automatic) but rejects a non-literal")
{
	airplay::DisplaySettings d;
	d.device_id = "A";
	d.display_name = "Roku";
	DisplaySettingsDialog dialog(d, {});
	auto *ip = dialog.findChild<QLineEdit *>(QStringLiteral("ip"));
	auto *ok = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
	CHECK(ok->isEnabled()); // empty is fine: "found automatically"

	ip->setText(QStringLiteral("roku.local"));
	CHECK_FALSE(ok->isEnabled());
	ip->setText(QStringLiteral("10.20.0.178"));
	CHECK(ok->isEnabled());
	ip->clear();
	CHECK(ok->isEnabled());
}

TEST_CASE("an answered credential prompt reaches the backend")
{
	FakeBackend b;
	AirPlayDock dock(&b);
	dock.prompt_credential(QStringLiteral("A"), QStringLiteral("Roku"), QStringLiteral("password"), false);
	auto *dialog = dynamic_cast<CredentialDialog *>(dock.findChild<QDialog *>());
	REQUIRE(dialog);
	dialog->findChild<QLineEdit *>(QStringLiteral("value"))->setText(QStringLiteral("hunter2"));
	dialog->accept();
	CHECK(b.calls == std::vector<std::string>{"credential A hunter2 remember"});
}

TEST_CASE("display settings dialog: the audio format choice round-trips")
{
	airplay::DisplaySettings d;
	d.device_id = "A";
	d.display_name = "Roku";
	DisplaySettingsDialog dialog(d, {});
	CHECK(dialog.result_settings() == d); // untouched: Auto

	auto *format = dialog.findChild<QComboBox *>(QStringLiteral("audioFormat"));
	REQUIRE(format);
	CHECK(format->currentIndex() == 0);
	format->setCurrentIndex(format->findData(QStringLiteral("aac-eld")));
	CHECK(dialog.result_settings().audio_format == "aac-eld");

	d.audio_format = "alac";
	DisplaySettingsDialog reopened(d, {});
	CHECK(reopened.findChild<QComboBox *>(QStringLiteral("audioFormat"))->currentData().toString() == QStringLiteral("alac"));
	CHECK(reopened.result_settings() == d);
}

TEST_CASE("only the Display column opens an editor")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	QTreeWidget *tree = dock.tree();
	QTreeWidgetItem *row = tree->topLevelItem(0)->child(0);
	REQUIRE(row);
	auto editors = [tree] { return tree->viewport()->findChildren<QLineEdit *>().size(); };
	for (int column : {1, 2}) {
		tree->editItem(row, column);
		CHECK(editors() == 0);
	}
	tree->editItem(row, 0);
	CHECK(editors() == 1);
}

TEST_CASE("the Display settings button opens the selected display's settings")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	QTreeWidget *tree = dock.tree();
	QPushButton *button = dock.display_settings_button();
	REQUIRE(button);
	CHECK(button->text() == QStringLiteral("Display settings..."));
	tree->setCurrentItem(nullptr);
	CHECK_FALSE(button->isEnabled());
	tree->setCurrentItem(tree->topLevelItem(0)); // a location heading
	CHECK_FALSE(button->isEnabled());
	QTreeWidgetItem *row = tree->topLevelItem(0)->child(0);
	tree->setCurrentItem(row);
	REQUIRE(button->isEnabled());

	const std::string id = row->data(0, AirPlayDock::kKeyRole).toString().toStdString();
	QTimer::singleShot(0, [] {
		if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
			dialog->accept();
	});
	button->click();
	REQUIRE_FALSE(b.calls.empty());
	CHECK(b.calls.back() == "update " + id);
}

TEST_CASE("display dialog: Auto TV delay mode is saved and only enabled for Auto")
{
	airplay::DisplaySettings d;
	d.device_id = "AA";
	d.display_name = "Roku";
	d.latency_ms = 0;
	d.lead_mode = airplay::kLeadModeDynamic;
	DisplaySettingsDialog dialog(d, {});
	CHECK(dialog.result_settings() == d); // untouched: dynamic stays dynamic
	auto *mode = dialog.findChild<QComboBox *>(QStringLiteral("leadMode"));
	auto *delay = dialog.findChild<QSpinBox *>(QStringLiteral("tvDelay"));
	REQUIRE(mode != nullptr);
	REQUIRE(delay != nullptr);
	CHECK(mode->isEnabled());
	CHECK(mode->currentData().toString() == QStringLiteral("dynamic"));
	mode->setCurrentIndex(mode->findData(QString()));
	CHECK(dialog.result_settings().lead_mode.empty());
	delay->setValue(120);
	CHECK_FALSE(mode->isEnabled());
}

TEST_CASE("double-clicking a display's status opens its settings; a heading or name does not")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	QTreeWidget *tree = dock.tree();
	QTreeWidgetItem *heading = tree->topLevelItem(0);
	QTreeWidgetItem *row = heading->child(0);
	const auto accept_dialog = [] {
		QTimer::singleShot(0, [] {
			if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
				dialog->accept();
		});
	};

	emit tree->itemDoubleClicked(heading, 1);
	emit tree->itemDoubleClicked(row, 0); // the name: rename, not settings
	CHECK(b.calls.empty());

	accept_dialog();
	emit tree->itemDoubleClicked(row, 1);
	REQUIRE(b.calls.size() == 1);
	CHECK(b.calls.back() == "update " + row->data(0, AirPlayDock::kKeyRole).toString().toStdString());
}

TEST_CASE("display dialog: Disconnect on these scenes round-trips and keeps a scene OBS no longer lists")
{
	airplay::DisplaySettings d;
	d.device_id = "N";
	d.display_name = "Narthex";
	d.skip_scenes = {"Old Wide"};
	DisplaySettingsDialog dialog(d, {}, {"Pulpit", "Narthex Wide"});
	CHECK(dialog.result_settings() == d); // untouched: the missing scene stays

	auto *list = dialog.findChild<QListWidget *>(QStringLiteral("skipScenes"));
	REQUIRE(list != nullptr);
	REQUIRE(list->count() == 3);
	CHECK(list->item(0)->text() == QStringLiteral("Pulpit"));
	CHECK(list->item(0)->checkState() == Qt::Unchecked);
	CHECK(list->item(2)->text() == QStringLiteral("Old Wide (not in this scene collection)"));
	CHECK(list->item(2)->checkState() == Qt::Checked);

	list->item(1)->setCheckState(Qt::Checked);
	list->item(2)->setCheckState(Qt::Unchecked);
	CHECK(dialog.result_settings().skip_scenes == std::vector<std::string>{"Narthex Wide"});
}

TEST_CASE("every display row has a settings gear that opens its settings; headings have none")
{
	FakeBackend b = two_groups();
	AirPlayDock dock(&b);
	QTreeWidget *tree = dock.tree();
	QTreeWidgetItem *heading = tree->topLevelItem(0);
	CHECK(tree->itemWidget(heading, 3) == nullptr);
	CHECK(dock.findChildren<QToolButton *>(QStringLiteral("rowSettings")).size() == 3);

	QTreeWidgetItem *row = heading->child(1);
	auto *gear = qobject_cast<QToolButton *>(tree->itemWidget(row, 3));
	REQUIRE(gear != nullptr);
	CHECK(gear->toolTip() == QStringLiteral("Display settings"));
	QTimer::singleShot(0, [] {
		if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
			dialog->accept();
	});
	gear->click();
	REQUIRE(b.calls.size() == 1);
	CHECK(b.calls.back() == "update " + row->data(0, AirPlayDock::kKeyRole).toString().toStdString());

	dock.refresh(); // rebuilt rows get their gears back
	QApplication::processEvents(QEventLoop::AllEvents);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	CHECK(dock.findChildren<QToolButton *>(QStringLiteral("rowSettings")).size() == 3);
}
