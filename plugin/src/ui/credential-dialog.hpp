// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLineEdit;

// Asks for the PIN a receiver shows on screen, or its AirPlay password.
class CredentialDialog : public QDialog {
public:
	CredentialDialog(const QString &display_name, const QString &kind, bool retry, bool can_remember,
			 QWidget *parent = nullptr);
	QString value() const;
	bool remember() const;

private:
	QLineEdit *value_ = nullptr;
	QCheckBox *remember_ = nullptr;
};
