// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#pragma once

#include "airplay/settings.hpp"

#include <QDialog>

#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

// Per-display settings: name, location, auto-reconnect, volume, manual
// address, Wi-Fi tolerance, TV delay, idle policy, and clearing the saved
// password.
class DisplaySettingsDialog : public QDialog {
public:
	DisplaySettingsDialog(const airplay::DisplaySettings &display, const std::vector<std::string> &locations,
			      QWidget *parent = nullptr);
	// Never reflects the "Forget the saved AirPlay password" checkbox; that is
	// reported separately by clear_password(), since clearing the password is
	// a distinct action (DockBackend::forget_password), not a settings field.
	airplay::DisplaySettings result_settings() const;
	bool clear_password() const;

private:
	airplay::DisplaySettings original_;
	QLineEdit *name_ = nullptr;
	QComboBox *location_ = nullptr;
	QCheckBox *auto_reconnect_ = nullptr;
	QCheckBox *send_audio_ = nullptr;
	QCheckBox *set_volume_ = nullptr;
	QDoubleSpinBox *volume_ = nullptr;
	QLineEdit *ip_ = nullptr;
	QSpinBox *port_ = nullptr;
	QCheckBox *wifi_tolerant_ = nullptr;
	QComboBox *audio_format_ = nullptr;
	QSpinBox *tv_delay_ = nullptr;
	QComboBox *lead_mode_ = nullptr;
	QComboBox *idle_ = nullptr;
	QSpinBox *idle_minutes_ = nullptr;
	QCheckBox *clear_password_ = nullptr;
};
