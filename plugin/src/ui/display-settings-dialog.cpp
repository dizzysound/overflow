// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "display-settings-dialog.hpp"

#include "airplay/lead_policy.hpp"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

using airplay::IdlePolicy;

namespace {

// Minor 3: mirrors AddDisplayDialog's check, but the field is optional here
// (empty means "found automatically").
bool valid_or_empty_ip(const QString &text)
{
	return text.isEmpty() || QHostAddress(text).protocol() != QAbstractSocket::UnknownNetworkLayerProtocol;
}

} // namespace

DisplaySettingsDialog::DisplaySettingsDialog(const airplay::DisplaySettings &display,
					     const std::vector<std::string> &locations, QWidget *parent)
	: QDialog(parent), original_(display)
{
	setWindowTitle(QStringLiteral("Display settings"));
	auto *form = new QFormLayout;

	name_ = new QLineEdit(QString::fromStdString(display.display_name));
	name_->setObjectName(QStringLiteral("name"));
	form->addRow(QStringLiteral("Name"), name_);

	location_ = new QComboBox;
	location_->setObjectName(QStringLiteral("location"));
	location_->setEditable(true);
	location_->addItem(QString());
	for (const std::string &l : locations)
		location_->addItem(QString::fromStdString(l));
	location_->setEditText(QString::fromStdString(display.location));
	location_->setToolTip(QStringLiteral("Displays with the same location are grouped in the dock, for example \"Friendship Hall\"."));
	form->addRow(QStringLiteral("Location"), location_);

	auto_reconnect_ = new QCheckBox(QStringLiteral("Reconnect automatically after a drop"));
	auto_reconnect_->setObjectName(QStringLiteral("autoReconnect"));
	auto_reconnect_->setChecked(display.auto_reconnect);
	form->addRow(auto_reconnect_);

	send_audio_ = new QCheckBox(QStringLiteral("Send audio to this display"));
	send_audio_->setObjectName(QStringLiteral("sendAudio"));
	send_audio_->setChecked(display.audio_enabled);
	send_audio_->setToolTip(QStringLiteral(
		"Off: this display gets video only. Its volume is never touched."));
	form->addRow(send_audio_);

	set_volume_ = new QCheckBox(QStringLiteral("Set the receiver volume when connecting"));
	set_volume_->setObjectName(QStringLiteral("setVolume"));
	set_volume_->setChecked(display.volume_db.has_value());
	volume_ = new QDoubleSpinBox;
	volume_->setObjectName(QStringLiteral("volume"));
	volume_->setRange(airplay::kMinVolumeDb, airplay::kMaxVolumeDb);
	volume_->setSingleStep(0.5);
	volume_->setDecimals(1);
	volume_->setSuffix(QStringLiteral(" dB"));
	volume_->setValue(display.volume_db.value_or(-15.0));
	auto update_volume_enabled = [this] {
		const bool audio_on = send_audio_->isChecked();
		set_volume_->setEnabled(audio_on);
		volume_->setEnabled(audio_on && set_volume_->isChecked());
	};
	update_volume_enabled();
	connect(set_volume_, &QCheckBox::toggled, this, update_volume_enabled);
	connect(send_audio_, &QCheckBox::toggled, this, update_volume_enabled);
	form->addRow(set_volume_);
	form->addRow(QStringLiteral("Volume"), volume_);
	auto *volume_note = new QLabel(QStringLiteral(
		"Unchecked, the plugin never changes this receiver's volume. Unchecking it later leaves the TV at its current level."));
	volume_note->setWordWrap(true);
	form->addRow(volume_note);

	ip_ = new QLineEdit(QString::fromStdString(display.manual_ip));
	ip_->setObjectName(QStringLiteral("ip"));
	ip_->setPlaceholderText(QStringLiteral("Found automatically"));
	port_ = new QSpinBox;
	port_->setObjectName(QStringLiteral("port"));
	port_->setRange(0, 65535);
	port_->setSpecialValueText(QStringLiteral("7000 (default)"));
	port_->setValue(display.manual_port);
	form->addRow(QStringLiteral("Manual IP address"), ip_);
	form->addRow(QStringLiteral("Port"), port_);

	wifi_tolerant_ = new QCheckBox(QStringLiteral("Wi-Fi display: tolerate brief network stalls"));
	wifi_tolerant_->setObjectName(QStringLiteral("wifiTolerant"));
	wifi_tolerant_->setChecked(display.wifi_tolerant);
	wifi_tolerant_->setToolTip(QStringLiteral(
		"Adds 50 ms to this display's TV delay so brief Wi-Fi hiccups do not freeze the picture. Leave off for wired displays."));
	form->addRow(wifi_tolerant_);

	tv_delay_ = new QSpinBox;
	tv_delay_->setObjectName(QStringLiteral("tvDelay"));
	// One step below the helper's 40 ms floor reads Auto (stored as 0), so a
	// fixed delay can never show a value the helper would silently raise.
	tv_delay_->setRange(airplay::kLeadFloorMs - 5, airplay::kLeadCeilingMs);
	tv_delay_->setSingleStep(5);
	tv_delay_->setSuffix(QStringLiteral(" ms"));
	tv_delay_->setSpecialValueText(QStringLiteral("Auto (measured)"));
	tv_delay_->setValue(display.latency_ms == 0 ? tv_delay_->minimum()
						     : std::clamp(display.latency_ms, airplay::kLeadFloorMs,
								  airplay::kLeadCeilingMs));
	tv_delay_->setToolTip(QStringLiteral(
		"How far ahead of presentation this TV's picture and sound are sent. Auto measures OBS audio and video "
		"and adds a margin (more for Wi-Fi displays). The TV's own picture processing adds to this. "
		"The smallest fixed delay is 40 ms. Too small a delay makes the sound break up: lower it in steps "
		"while listening, and watch the audio loss shown in the dock."));
	form->addRow(QStringLiteral("TV delay"), tv_delay_);

	lead_mode_ = new QComboBox;
	lead_mode_->setObjectName(QStringLiteral("leadMode"));
	lead_mode_->addItem(QStringLiteral("Raise only during a service"), QString());
	lead_mode_->addItem(QStringLiteral("Dynamic: also lower while clean"), QStringLiteral("dynamic"));
	lead_mode_->setCurrentIndex(std::max(0, lead_mode_->findData(QString::fromStdString(display.lead_mode))));
	lead_mode_->setToolTip(QStringLiteral("How Auto changes this TV's delay during a service, without reconnecting"));
	const auto sync_mode = [this] { lead_mode_->setEnabled(tv_delay_->value() == tv_delay_->minimum()); };
	connect(tv_delay_, qOverload<int>(&QSpinBox::valueChanged), this, sync_mode);
	sync_mode();
	form->addRow(QStringLiteral("Auto TV delay"), lead_mode_);

	audio_format_ = new QComboBox;
	audio_format_->setObjectName(QStringLiteral("audioFormat"));
	audio_format_->addItem(QStringLiteral("Auto (what the TV advertises)"), QString());
	audio_format_->addItem(QStringLiteral("ALAC (lossless)"), QStringLiteral("alac"));
	audio_format_->addItem(QStringLiteral("AAC-ELD"), QStringLiteral("aac-eld"));
	audio_format_->setCurrentIndex(std::max(0, audio_format_->findData(QString::fromStdString(display.audio_format))));
	audio_format_->setToolTip(QStringLiteral(
		"Leave on Auto. Forcing a format the TV does not list is a listening test: if the TV cannot decode it, "
		"its sound is noise or silent, or the session drops. Takes effect when the display reconnects."));
	form->addRow(QStringLiteral("Audio format"), audio_format_);

	idle_ = new QComboBox;
	idle_->setObjectName(QStringLiteral("idle"));
	idle_->addItem(QStringLiteral("Stay connected"), static_cast<int>(IdlePolicy::StayConnected));
	idle_->addItem(QStringLiteral("Disconnect after minutes idle"), static_cast<int>(IdlePolicy::DisconnectAfterIdle));
	idle_->addItem(QStringLiteral("Disconnect when AirPlay stops"),
		       static_cast<int>(IdlePolicy::DisconnectWhenOutputStops));
	idle_->setCurrentIndex(idle_->findData(static_cast<int>(display.idle_policy)));
	idle_minutes_ = new QSpinBox;
	idle_minutes_->setObjectName(QStringLiteral("idleMinutes"));
	idle_minutes_->setRange(1, 1440);
	idle_minutes_->setSuffix(QStringLiteral(" min"));
	idle_minutes_->setValue(display.idle_minutes);
	auto update_minutes = [this] {
		idle_minutes_->setEnabled(idle_->currentData().toInt() == static_cast<int>(IdlePolicy::DisconnectAfterIdle));
	};
	update_minutes();
	connect(idle_, &QComboBox::currentIndexChanged, this, update_minutes);
	form->addRow(QStringLiteral("When idle"), idle_);
	form->addRow(QStringLiteral("Idle minutes"), idle_minutes_);
	auto *idle_note = new QLabel(QStringLiteral(
		"Idle means OBS is neither streaming nor recording and the program scene has not changed."));
	idle_note->setWordWrap(true);
	form->addRow(idle_note);

	clear_password_ = new QCheckBox(QStringLiteral("Forget the saved AirPlay password"));
	clear_password_->setObjectName(QStringLiteral("clearPassword"));
	clear_password_->setVisible(!display.password_protected.empty());
	form->addRow(clear_password_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	QPushButton *ok = buttons->button(QDialogButtonBox::Ok);
	ok->setEnabled(valid_or_empty_ip(ip_->text().trimmed()));
	connect(ip_, &QLineEdit::textChanged, ok, [ok](const QString &text) { ok->setEnabled(valid_or_empty_ip(text.trimmed())); });
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(buttons);
}

airplay::DisplaySettings DisplaySettingsDialog::result_settings() const
{
	airplay::DisplaySettings r = original_;
	const std::string name = name_->text().trimmed().toStdString();
	if (!name.empty())
		r.display_name = name;
	r.location = location_->currentText().trimmed().toStdString();
	r.auto_reconnect = auto_reconnect_->isChecked();
	r.audio_enabled = send_audio_->isChecked();
	r.volume_db = set_volume_->isChecked() ? std::optional<double>(volume_->value()) : std::nullopt;
	r.manual_ip = ip_->text().trimmed().toStdString();
	r.manual_port = r.manual_ip.empty() ? 0 : port_->value();
	r.wifi_tolerant = wifi_tolerant_->isChecked();
	r.latency_ms = tv_delay_->value() == tv_delay_->minimum() ? 0 : tv_delay_->value();
	r.lead_mode = lead_mode_->currentData().toString().toStdString();
	r.audio_format = audio_format_->currentData().toString().toStdString();
	r.idle_policy = static_cast<IdlePolicy>(idle_->currentData().toInt());
	r.idle_minutes = idle_minutes_->value();
	return r;
}

bool DisplaySettingsDialog::clear_password() const
{
	return clear_password_->isChecked();
}
