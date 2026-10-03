// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "global-settings-dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>

using airplay::QualityPreset;
using airplay::TimingMode;

GlobalSettingsDialog::GlobalSettingsDialog(const airplay::GlobalSettings &global,
					   const std::vector<std::string> &encoders, QWidget *parent,
					   std::function<std::string()> diagnostics)
	: QDialog(parent), original_(global), diagnostics_(std::move(diagnostics))
{
	setWindowTitle(QStringLiteral("Overflow settings"));

	auto *start_box = new QGroupBox(QStringLiteral("Start the displays automatically"));
	auto *start_layout = new QVBoxLayout(start_box);
	on_launch_ = new QCheckBox(QStringLiteral("When OBS starts"));
	on_launch_->setObjectName(QStringLiteral("onLaunch"));
	on_launch_->setChecked(global.start_on_launch);
	with_streaming_ = new QCheckBox(QStringLiteral("With streaming (and stop when streaming stops)"));
	with_streaming_->setObjectName(QStringLiteral("withStreaming"));
	with_streaming_->setChecked(global.start_with_streaming);
	with_recording_ = new QCheckBox(QStringLiteral("With recording (and stop when recording stops)"));
	with_recording_->setObjectName(QStringLiteral("withRecording"));
	with_recording_->setChecked(global.start_with_recording);
	start_layout->addWidget(on_launch_);
	start_layout->addWidget(with_streaming_);
	start_layout->addWidget(with_recording_);

	auto *form = new QFormLayout;
	track_ = new QComboBox;
	track_->setObjectName(QStringLiteral("track"));
	for (int t = 1; t <= 6; ++t)
		track_->addItem(QStringLiteral("Track %1").arg(t), t);
	track_->setCurrentIndex(std::clamp(global.audio_track, 1, 6) - 1);
	form->addRow(QStringLiteral("Audio"), track_);

	// Controller ruling R1: one shared encoder, set globally. Only these four
	// presets exist (airplay::QualityPreset); the default is P1080_6Mbps.
	preset_ = new QComboBox;
	preset_->setObjectName(QStringLiteral("preset"));
	preset_->addItem(QStringLiteral("1080p, 6 Mbps (default)"), static_cast<int>(QualityPreset::P1080_6Mbps));
	preset_->addItem(QStringLiteral("1080p, 8 Mbps"), static_cast<int>(QualityPreset::P1080_8Mbps));
	preset_->addItem(QStringLiteral("1080p, 10 Mbps"), static_cast<int>(QualityPreset::P1080_10Mbps));
	preset_->addItem(QStringLiteral("720p, 4 Mbps"), static_cast<int>(QualityPreset::P720_4Mbps));
	preset_->setCurrentIndex(preset_->findData(static_cast<int>(global.preset)));
	form->addRow(QStringLiteral("Video"), preset_);

	auto *advanced = new QGroupBox(QStringLiteral("Advanced"));
	auto *adv_form = new QFormLayout(advanced);
	encoder_ = new QComboBox;
	encoder_->setObjectName(QStringLiteral("encoder"));
	encoder_->addItem(QStringLiteral("Automatic (NVENC, AMF, Quick Sync, x264)"), QString());
	for (const std::string &id : encoders)
		encoder_->addItem(QString::fromStdString(id), QString::fromStdString(id));
	if (!global.encoder_override.empty() &&
	    std::find(encoders.begin(), encoders.end(), global.encoder_override) == encoders.end())
		encoder_->addItem(QString::fromStdString(global.encoder_override + " (not available)"),
				  QString::fromStdString(global.encoder_override));
	encoder_->setCurrentIndex(std::max(0, encoder_->findData(QString::fromStdString(global.encoder_override))));
	adv_form->addRow(QStringLiteral("Encoder"), encoder_);

	latency_ = new QSpinBox;
	latency_->setObjectName(QStringLiteral("latency"));
	latency_->setRange(0, 2000);
	latency_->setSingleStep(50);
	latency_->setSuffix(QStringLiteral(" ms"));
	latency_->setSpecialValueText(QStringLiteral("Auto (match OBS audio)"));
	latency_->setToolTip(QStringLiteral(
		"Auto sets the TV delay to the smallest value that keeps audio in sync with OBS. Set a value to override it."));
	latency_->setValue(global.target_latency_ms);
	adv_form->addRow(QStringLiteral("Target latency"), latency_);

	timing_ = new QComboBox;
	timing_->setObjectName(QStringLiteral("timing"));
	timing_->addItem(QStringLiteral("Automatic (PTP or NTP)"), static_cast<int>(TimingMode::Auto));
	timing_->addItem(QStringLiteral("NTP for every display"), static_cast<int>(TimingMode::Ntp));
	timing_->setCurrentIndex(timing_->findData(static_cast<int>(global.timing)));
	adv_form->addRow(QStringLiteral("Timing"), timing_);

	eld_ = new QCheckBox(QStringLiteral("Use eld-encoder for receivers that only take AAC-ELD audio"));
	eld_->setObjectName(QStringLiteral("eld"));
	eld_->setChecked(global.eld_encoder);
	adv_form->addRow(eld_);

	verbose_log_ = new QCheckBox(QStringLiteral("Verbose helper logging (for troubleshooting)"));
	verbose_log_->setObjectName(QStringLiteral("verboseLog"));
	verbose_log_->setChecked(global.verbose_helper_log);
	adv_form->addRow(verbose_log_);

	auto *note = new QLabel(QStringLiteral("Changing latency, timing, eld-encoder or verbose helper logging restarts the helper."));
	note->setWordWrap(true);
	adv_form->addRow(note);

	auto *save_log = new QPushButton(QStringLiteral("Save diagnostic log..."));
	save_log->setObjectName(QStringLiteral("saveDiagnosticLog"));
	connect(save_log, &QPushButton::clicked, this, &GlobalSettingsDialog::save_diagnostic_log);
	adv_form->addRow(save_log);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(start_box);
	layout->addLayout(form);
	layout->addWidget(advanced);
	layout->addWidget(buttons);
}

airplay::GlobalSettings GlobalSettingsDialog::result_settings() const
{
	airplay::GlobalSettings g = original_;
	g.audio_track = track_->currentData().toInt();
	g.preset = static_cast<QualityPreset>(preset_->currentData().toInt());
	g.encoder_override = encoder_->currentData().toString().toStdString();
	g.start_on_launch = on_launch_->isChecked();
	g.start_with_streaming = with_streaming_->isChecked();
	g.start_with_recording = with_recording_->isChecked();
	g.target_latency_ms = latency_->value();
	g.timing = static_cast<TimingMode>(timing_->currentData().toInt());
	g.eld_encoder = eld_->isChecked();
	g.verbose_helper_log = verbose_log_->isChecked();
	return g;
}

void GlobalSettingsDialog::save_diagnostic_log()
{
	if (!diagnostics_) {
		QMessageBox::warning(this, QStringLiteral("Save diagnostic log"),
				     QStringLiteral("No diagnostics report is available."));
		return;
	}
	const QString default_name =
		QStringLiteral("overflow-diagnostics-%1.txt").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
	const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	const QString default_path = documents.isEmpty() ? default_name : documents + QStringLiteral("/") + default_name;
	const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save diagnostic log"), default_path,
							   QStringLiteral("Text files (*.txt)"));
	if (path.isEmpty())
		return;

	const std::string report = diagnostics_();
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QMessageBox::warning(this, QStringLiteral("Save diagnostic log"),
				     QStringLiteral("Could not write %1: %2").arg(path, file.errorString()));
		return;
	}
	QTextStream stream(&file);
	stream << QString::fromStdString(report);
	file.close();
	QMessageBox::information(this, QStringLiteral("Save diagnostic log"),
				 QStringLiteral("Diagnostic log saved to %1").arg(path));
}
