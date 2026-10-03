// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#pragma once

#include <QDialog>
#include <QString>

#include <string>
#include <vector>

class QComboBox;
class QLineEdit;
class QSpinBox;

// Adds a display that discovery cannot see, by IP address.
class AddDisplayDialog : public QDialog {
public:
	explicit AddDisplayDialog(const std::vector<std::string> &locations, QWidget *parent = nullptr);
	QString name() const;
	QString ip() const;
	int port() const;
	QString location() const;

private:
	QLineEdit *name_ = nullptr;
	QLineEdit *ip_ = nullptr;
	QSpinBox *port_ = nullptr;
	QComboBox *location_ = nullptr;
};
