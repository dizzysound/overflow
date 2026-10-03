// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "add-display-dialog.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHostAddress>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

// Minor 3: the helper ignores a non-literal ip and logs it, leaving the
// display stuck at "manual" with no error visible in the dock; refuse a
// host name or typo here instead.
bool valid_ip(const QString &text)
{
	return QHostAddress(text).protocol() != QAbstractSocket::UnknownNetworkLayerProtocol;
}

} // namespace

AddDisplayDialog::AddDisplayDialog(const std::vector<std::string> &locations, QWidget *parent) : QDialog(parent)
{
	setWindowTitle(QStringLiteral("Add display"));
	auto *form = new QFormLayout;
	name_ = new QLineEdit;
	name_->setObjectName(QStringLiteral("name"));
	ip_ = new QLineEdit;
	ip_->setObjectName(QStringLiteral("ip"));
	ip_->setPlaceholderText(QStringLiteral("10.20.0.178"));
	port_ = new QSpinBox;
	port_->setObjectName(QStringLiteral("port"));
	port_->setRange(0, 65535);
	port_->setSpecialValueText(QStringLiteral("7000 (default)"));
	location_ = new QComboBox;
	location_->setObjectName(QStringLiteral("location"));
	location_->setEditable(true);
	location_->addItem(QString());
	for (const std::string &l : locations)
		location_->addItem(QString::fromStdString(l));
	form->addRow(QStringLiteral("Name"), name_);
	form->addRow(QStringLiteral("IP address"), ip_);
	form->addRow(QStringLiteral("Port"), port_);
	form->addRow(QStringLiteral("Location"), location_);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	QPushButton *ok = buttons->button(QDialogButtonBox::Ok);
	ok->setEnabled(false);
	connect(ip_, &QLineEdit::textChanged, ok, [ok](const QString &text) { ok->setEnabled(valid_ip(text.trimmed())); });
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(buttons);
}

QString AddDisplayDialog::name() const
{
	return name_->text().trimmed();
}

QString AddDisplayDialog::ip() const
{
	return ip_->text().trimmed();
}

int AddDisplayDialog::port() const
{
	return port_->value();
}

QString AddDisplayDialog::location() const
{
	return location_->currentText().trimmed();
}
