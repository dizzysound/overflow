// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "credential-dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

CredentialDialog::CredentialDialog(const QString &display_name, const QString &kind, bool retry, bool can_remember,
				   QWidget *parent)
	: QDialog(parent)
{
	const bool password = kind == QStringLiteral("password");
	setWindowTitle(password ? QStringLiteral("AirPlay password") : QStringLiteral("AirPlay PIN"));

	QString text;
	if (retry)
		text = password ? QStringLiteral("The password was not accepted. ")
				: QStringLiteral("The code was not accepted. ");
	text += password ? QStringLiteral("Enter the AirPlay password for %1.").arg(display_name)
			 : QStringLiteral("Enter the code shown on the %1 TV.").arg(display_name);
	auto *label = new QLabel(text);
	label->setObjectName(QStringLiteral("prompt"));
	label->setWordWrap(true);

	value_ = new QLineEdit;
	value_->setObjectName(QStringLiteral("value"));
	if (password)
		value_->setEchoMode(QLineEdit::Password);

	remember_ = new QCheckBox(QStringLiteral("Remember this password on this computer"));
	remember_->setObjectName(QStringLiteral("remember"));
	remember_->setChecked(password && can_remember);
	remember_->setVisible(password && can_remember);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(label);
	layout->addWidget(value_);
	layout->addWidget(remember_);
	layout->addWidget(buttons);
}

QString CredentialDialog::value() const
{
	return value_->text().trimmed();
}

bool CredentialDialog::remember() const
{
	return remember_->isChecked();
}
