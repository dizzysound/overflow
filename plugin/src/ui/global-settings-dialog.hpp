// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#pragma once

#include "airplay/settings.hpp"

#include <QDialog>

#include <functional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QSpinBox;

class GlobalSettingsDialog : public QDialog {
public:
	// diagnostics, when set, backs the "Save diagnostic log..." button:
	// called on demand to get the report text to write. Left empty (the
	// default), the button still shows but reports that no report is
	// available, so existing callers/tests that only pass global+encoders
	// keep an unchanged round trip.
	GlobalSettingsDialog(const airplay::GlobalSettings &global, const std::vector<std::string> &encoders,
			     QWidget *parent = nullptr, std::function<std::string()> diagnostics = {});
	airplay::GlobalSettings result_settings() const;

private:
	void save_diagnostic_log();

	airplay::GlobalSettings original_;
	std::function<std::string()> diagnostics_;
	QComboBox *track_ = nullptr;
	QComboBox *preset_ = nullptr;
	QComboBox *encoder_ = nullptr;
	QCheckBox *on_launch_ = nullptr;
	QCheckBox *with_streaming_ = nullptr;
	QCheckBox *with_recording_ = nullptr;
	QSpinBox *latency_ = nullptr;
	QComboBox *timing_ = nullptr;
	QCheckBox *eld_ = nullptr;
	QCheckBox *verbose_log_ = nullptr;
};
