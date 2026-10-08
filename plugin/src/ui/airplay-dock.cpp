// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay-dock.hpp"

#include "add-display-dialog.hpp"
#include "credential-dialog.hpp"
#include "display-settings-dialog.hpp"
#include "global-settings-dialog.hpp"

#include <QAction>
#include <QColor>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextStream>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace {

enum Column { kName = 0, kStatus = 1, kAudio = 2, kSettings = 3 };

constexpr int kMinDataColumn = 90; // Display, Status and Audio never shrink below this
constexpr int kSettingsColumn = 34;

// QAbstractItemView::state() is protected; the dock needs to know whether a
// name is being edited so a refresh does not clobber it.
class DisplayTree final : public QTreeWidget {
public:
	using QTreeWidget::QTreeWidget;
	bool editing() const { return state() == QAbstractItemView::EditingState; }

protected:
	// Only the display name is editable (rename in place). Item flags apply to
	// every column, so Status and Audio would otherwise open an editor too.
	bool edit(const QModelIndex &index, EditTrigger trigger, QEvent *event) override
	{
		if (index.column() != kName)
			return false;
		return QTreeWidget::edit(index, trigger, event);
	}
};

QIcon light_icon(airplay::Light light)
{
	QColor color(0x8a, 0x8a, 0x8a); // gray: idle, offline
	switch (light) {
	case airplay::Light::Yellow:
		color = QColor(0xe0, 0xb0, 0x00);
		break;
	case airplay::Light::Green:
		color = QColor(0x2e, 0x9e, 0x44);
		break;
	case airplay::Light::Red:
		color = QColor(0xd0, 0x30, 0x30);
		break;
	case airplay::Light::Gray:
		break;
	}
	QPixmap pixmap(12, 12);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(color);
	painter.drawEllipse(1, 1, 10, 10);
	return QIcon(pixmap);
}

QString state_text(const airplay::DisplayRow &row)
{
	if (!row.state.empty())
		return QString::fromStdString(row.state);
	if (row.discovered)
		return QStringLiteral("available");
	return row.manual ? QStringLiteral("manual") : QStringLiteral("not found");
}

QString display_tooltip(const airplay::DisplayRow &row)
{
	// Leads with the full display name: the Display column elides long names
	// (for example "Main Room Center Screen" as "Sanctu...") once it is
	// narrower than the text, and this is the only place the whole name
	// still shows.
	QString tip = QString::fromStdString(row.display_name);
	tip += QStringLiteral("\n") + QString::fromStdString(row.device_id);
	if (!row.model.empty())
		tip += QStringLiteral("\n") + QString::fromStdString(row.model);
	if (!row.ip.empty())
		tip += QStringLiteral("\n") + QString::fromStdString(row.ip);
	return tip;
}

Qt::CheckState check_state(airplay::GroupCheck check)
{
	switch (check) {
	case airplay::GroupCheck::All:
		return Qt::Checked;
	case airplay::GroupCheck::Some:
		return Qt::PartiallyChecked;
	case airplay::GroupCheck::None:
		break;
	}
	return Qt::Unchecked;
}

QTreeWidgetItem *make_display_item(const airplay::DisplayRow &row)
{
	auto *item = new QTreeWidgetItem;
	item->setData(kName, AirPlayDock::kKeyRole, QString::fromStdString(row.device_id));
	item->setData(kName, AirPlayDock::kHeadingRole, false);
	item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
	item->setText(kName, QString::fromStdString(row.display_name));
	item->setCheckState(kName, row.enabled ? Qt::Checked : Qt::Unchecked);
	item->setToolTip(kName, display_tooltip(row));
	item->setIcon(kStatus, light_icon(row.light));
	item->setText(kStatus, state_text(row));
	item->setToolTip(kStatus, QString::fromStdString(row.status_tooltip));
	item->setText(kAudio, row.audio_off ? QStringLiteral("no audio") : QString());
	item->setToolTip(kAudio, QString::fromStdString(row.audio_reason));
	return item;
}

// OBS's own gear (its resources live in the OBS process; the theme's
// icon-gear class restyles it). Outside OBS, as in the UI tests, a text gear.
QToolButton *make_settings_button()
{
	auto *button = new QToolButton;
	button->setObjectName(QStringLiteral("rowSettings"));
	button->setAutoRaise(true);
	button->setToolTip(QStringLiteral("Display settings"));
	button->setAccessibleName(QStringLiteral("Display settings"));
	const QString gear = QStringLiteral(":/settings/images/settings/general.svg");
	if (QFile::exists(gear)) {
		button->setIcon(QIcon(gear));
		button->setProperty("class", QStringLiteral("icon-gear"));
	} else {
		button->setText(QString(QChar(0x2699)));
	}
	return button;
}

QString item_key(const QTreeWidgetItem *item)
{
	return item->data(kName, AirPlayDock::kKeyRole).toString();
}

bool is_heading(const QTreeWidgetItem *item)
{
	return item->data(kName, AirPlayDock::kHeadingRole).toBool();
}

} // namespace

AirPlayDock::AirPlayDock(DockBackend *backend, QWidget *parent) : QWidget(parent), backend_(backend)
{
	start_stop_ = new QPushButton(QStringLiteral("Start"));
	start_stop_->setObjectName(QStringLiteral("startStop"));
	auto *add = new QPushButton(QStringLiteral("Add display..."));
	add->setObjectName(QStringLiteral("addDisplay"));
	display_settings_ = new QPushButton(QStringLiteral("Display settings..."));
	display_settings_->setObjectName(QStringLiteral("displaySettingsButton"));
	display_settings_->setToolTip(
		QStringLiteral("Settings for the selected display (or its gear, or double-click its status)"));
	display_settings_->setEnabled(false);
	auto *settings = new QPushButton(QStringLiteral("Settings..."));
	settings->setObjectName(QStringLiteral("settings"));
	settings->setToolTip(QStringLiteral("Settings for all displays"));
	auto *bar = new QHBoxLayout;
	bar->addWidget(start_stop_);
	bar->addStretch();
	bar->addWidget(add);
	bar->addWidget(display_settings_);
	bar->addWidget(settings);

	status_ = new QLabel;
	status_->setObjectName(QStringLiteral("status"));
	status_->setWordWrap(true);

	tree_ = new DisplayTree;
	tree_->setObjectName(QStringLiteral("displays"));
	tree_->setColumnCount(4);
	tree_->setHeaderLabels({QStringLiteral("Display"), QStringLiteral("Status"), QStringLiteral("Audio"), QString()});
	tree_->setContextMenuPolicy(Qt::CustomContextMenu);
	tree_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	// Display and Status are user-resizable (Interactive); Audio stretches to
	// take the remainder instead of Display swallowing it (that left Display
	// unresizable and squeezed to nothing in a narrow dock). The last column
	// is each display's settings button, at a fixed width. Widths set here
	// are just the initial layout: refresh() never touches them, so a resize
	// the operator makes survives.
	QHeaderView *header = tree_->header();
	header->setSectionResizeMode(kName, QHeaderView::Interactive);
	header->setSectionResizeMode(kStatus, QHeaderView::Interactive);
	header->setSectionResizeMode(kAudio, QHeaderView::Stretch);
	header->setSectionResizeMode(kSettings, QHeaderView::Fixed);
	header->setStretchLastSection(false);
	// The header's minimum applies to every column, so it is the settings
	// column's width; the data columns keep their own larger minimum here.
	header->setMinimumSectionSize(kSettingsColumn);
	header->resizeSection(kSettings, kSettingsColumn);
	connect(header, &QHeaderView::sectionResized, this, [header](int index, int, int size) {
		if (index != kSettings && size < kMinDataColumn)
			header->resizeSection(index, kMinDataColumn);
	});
	header->setSectionsMovable(false);
	tree_->setColumnWidth(kName, 240);
	tree_->setColumnWidth(kStatus, 170);

	hint_ = new QLabel;
	hint_->setObjectName(QStringLiteral("hint"));
	hint_->setWordWrap(true);
	QFont hint_font = hint_->font();
	hint_font.setItalic(true);
	hint_->setFont(hint_font);
	QPalette hint_palette = hint_->palette();
	hint_palette.setColor(QPalette::WindowText, hint_palette.color(QPalette::Disabled, QPalette::WindowText));
	hint_->setPalette(hint_palette);

	setMinimumSize(420, 260);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(bar);
	layout->addWidget(status_);
	layout->addWidget(tree_);
	layout->addWidget(hint_);

	connect(start_stop_, &QPushButton::clicked, this, [this] {
		if (backend_->running())
			backend_->user_stop();
		else
			backend_->user_start();
		refresh();
	});
	connect(add, &QPushButton::clicked, this, [this] { add_display(); });
	connect(settings, &QPushButton::clicked, this, [this] { open_global_settings(); });
	connect(display_settings_, &QPushButton::clicked, this, [this] {
		const QTreeWidgetItem *current = tree_->currentItem();
		if (current && !is_heading(current))
			open_display_settings(item_key(current).toStdString());
	});
	connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *current) {
		display_settings_->setEnabled(current && !is_heading(current));
	});
	// Double-clicking a display's Status or Audio opens its settings; on its
	// name, a double-click renames it instead.
	connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
		if ((column == kStatus || column == kAudio) && !is_heading(item))
			open_display_settings(item_key(item).toStdString());
	});
	connect(tree_, &QTreeWidget::itemChanged, this,
		[this](QTreeWidgetItem *item, int column) { on_item_changed(item, column); });
	connect(tree_, &QTreeWidget::customContextMenuRequested, this,
		[this](const QPoint &pos) { show_context_menu(pos); });

	refresh();
}

void AirPlayDock::refresh()
{
	status_->setText(QString::fromStdString(backend_->status_text()));
	start_stop_->setText(backend_->running() ? QStringLiteral("Stop") : QStringLiteral("Start"));
	if (static_cast<DisplayTree *>(tree_)->editing())
		return; // the controller emits rows_changed again after the rename lands

	refreshing_ = true;
	QString current_key;
	bool current_is_heading = false;
	if (QTreeWidgetItem *current = tree_->currentItem()) {
		current_key = item_key(current);
		current_is_heading = is_heading(current);
	}
	const int scroll = tree_->verticalScrollBar()->value();

	tree_->clear();
	QTreeWidgetItem *restore = nullptr;
	for (const airplay::DisplayGroup &group : backend_->groups()) {
		QTreeWidgetItem *parent = nullptr;
		if (!group.location.empty()) {
			parent = new QTreeWidgetItem;
			const QString location = QString::fromStdString(group.location);
			parent->setData(kName, kKeyRole, location);
			parent->setData(kName, kHeadingRole, true);
			parent->setFlags((parent->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
			parent->setText(kName, location);
			parent->setCheckState(kName, check_state(group.check));
			parent->setToolTip(kName, QStringLiteral("Select or deselect every display in %1").arg(location));
			QFont bold = parent->font(kName);
			bold.setBold(true);
			parent->setFont(kName, bold);
			tree_->addTopLevelItem(parent);
			parent->setExpanded(true);
			if (current_is_heading && current_key == location)
				restore = parent;
		}
		for (const airplay::DisplayRow &row : group.displays) {
			QTreeWidgetItem *item = make_display_item(row);
			if (parent)
				parent->addChild(item);
			else
				tree_->addTopLevelItem(item);
			QToolButton *settings = make_settings_button();
			const std::string id = row.device_id;
			connect(settings, &QToolButton::clicked, this, [this, id] { open_display_settings(id); });
			tree_->setItemWidget(item, kSettings, settings);
			if (!current_is_heading && current_key == QString::fromStdString(row.device_id))
				restore = item;
		}
	}
	if (restore)
		tree_->setCurrentItem(restore);
	tree_->verticalScrollBar()->setValue(scroll);

	hint_->setText(tree_->topLevelItemCount() == 0
			       ? QStringLiteral("No displays found yet. They appear here as they are discovered, "
						 "or use Add display... to enter one by address.")
			       : QStringLiteral("Check a display to send to it. The gear opens its settings; "
						 "right-click for Restart or Forget. Double-click a name to rename it."));
	refreshing_ = false;
}

void AirPlayDock::on_item_changed(QTreeWidgetItem *item, int column)
{
	if (refreshing_ || column != kName)
		return;
	if (is_heading(item)) {
		// Clicking a partly checked heading checks it; either way the whole
		// location follows the heading.
		backend_->set_location_enabled(item_key(item).toStdString(), item->checkState(kName) == Qt::Checked);
		return;
	}
	const std::string id = item_key(item).toStdString();
	const std::vector<airplay::DisplayRow> rows = backend_->rows();
	const auto row =
		std::find_if(rows.begin(), rows.end(), [&](const airplay::DisplayRow &r) { return r.device_id == id; });
	if (row == rows.end())
		return;
	const bool checked = item->checkState(kName) == Qt::Checked;
	const std::string name = item->text(kName).trimmed().toStdString();
	if (checked != row->enabled)
		backend_->set_display_enabled(id, checked);
	if (!name.empty() && name != row->display_name)
		backend_->rename_display(id, name);
}

QMenu *AirPlayDock::build_display_menu(const std::string &id)
{
	auto *menu = new QMenu(this);
	QAction *settings = menu->addAction(QStringLiteral("Display settings..."));
	settings->setObjectName(QStringLiteral("displaySettings"));
	QAction *restart = menu->addAction(QStringLiteral("Restart"));
	restart->setObjectName(QStringLiteral("restart"));
	restart->setToolTip(QStringLiteral("Start a new session: use when a TV woken from standby has sound but no picture."));
	QAction *reconnect = menu->addAction(QStringLiteral("Reconnect"));
	reconnect->setObjectName(QStringLiteral("reconnect"));
	menu->addSeparator();
	QAction *forget = menu->addAction(QStringLiteral("Forget pairing..."));
	forget->setObjectName(QStringLiteral("forgetPairing"));
	QAction *remove = menu->addAction(QStringLiteral("Remove display..."));
	remove->setObjectName(QStringLiteral("removeDisplay"));

	connect(settings, &QAction::triggered, this, [this, id] { open_display_settings(id); });
	connect(restart, &QAction::triggered, this, [this, id] { backend_->restart_display(id); });
	connect(reconnect, &QAction::triggered, this, [this, id] { backend_->reconnect_display(id); });
	connect(forget, &QAction::triggered, this, [this, id] {
		const auto answer = QMessageBox::question(
			this, QStringLiteral("Forget pairing"),
			QStringLiteral("Forget the stored pairing and saved password for this display? It will ask "
					"for a code again next time. The display stays in this list with its "
					"other settings."));
		if (answer == QMessageBox::Yes)
			backend_->forget_display(id);
	});
	connect(remove, &QAction::triggered, this, [this, id] {
		const auto answer = QMessageBox::question(
			this, QStringLiteral("Remove display"),
			QStringLiteral("Remove this display from the list? Its name, location and other settings "
					"are lost. This does not affect AirPlay pairing on the receiver itself."));
		if (answer == QMessageBox::Yes)
			backend_->remove_display(id);
	});
	return menu;
}

void AirPlayDock::show_context_menu(const QPoint &pos)
{
	QTreeWidgetItem *item = tree_->itemAt(pos);
	if (!item || is_heading(item))
		return;
	QMenu *menu = build_display_menu(item_key(item).toStdString());
	menu->exec(tree_->viewport()->mapToGlobal(pos));
	menu->deleteLater();
}

void AirPlayDock::open_display_settings(const std::string &device_id)
{
	airplay::DisplaySettings display;
	if (const airplay::DisplaySettings *saved = backend_->settings().find_display(device_id)) {
		display = *saved;
	} else {
		display.device_id = device_id;
		for (const airplay::DisplayRow &row : backend_->rows())
			if (row.device_id == device_id)
				display.display_name = row.display_name;
	}
	DisplaySettingsDialog dialog(display, backend_->locations(), backend_->scene_names(), this);
	if (dialog.exec() == QDialog::Accepted) {
		backend_->update_display(dialog.result_settings());
		if (dialog.clear_password())
			backend_->forget_password(device_id);
	}
}

void AirPlayDock::open_global_settings()
{
	GlobalSettingsDialog dialog(backend_->settings().global, backend_->available_encoders(), this,
				    [this] { return backend_->diagnostics_report(); });
	if (dialog.exec() == QDialog::Accepted)
		backend_->update_global(dialog.result_settings());
}

void AirPlayDock::add_display()
{
	AddDisplayDialog dialog(backend_->locations(), this);
	if (dialog.exec() == QDialog::Accepted && !dialog.ip().isEmpty())
		backend_->add_display(dialog.name().toStdString(), dialog.ip().toStdString(), dialog.port(),
				      dialog.location().toStdString());
}

void AirPlayDock::save_diagnostic_log()
{
	const QString default_name = QStringLiteral("overflow-diagnostics-%1.txt")
					      .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
	const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	const QString default_path = documents.isEmpty() ? default_name : documents + QStringLiteral("/") + default_name;
	const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save diagnostic log"), default_path,
							   QStringLiteral("Text files (*.txt)"));
	if (path.isEmpty())
		return;

	const std::string report = backend_->diagnostics_report();
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

void AirPlayDock::prompt_credential(const QString &device_id, const QString &display_name, const QString &kind,
				    bool retry)
{
	// open() is window-modal: on macOS a sheet on the parent's window. OBS
	// keeps a dock it has never shown as a hidden floating window, and a sheet
	// on that makes AppKit show it while Qt still thinks it is hidden, which
	// leaves an empty "Overflow" window behind. Anchor on the main window then.
	QWidget *parent = this;
	if (!window()->isVisible() && window()->parentWidget())
		parent = window()->parentWidget()->window();
	auto *dialog = new CredentialDialog(display_name, kind, retry, backend_->can_remember_passwords(), parent);
	dialog->setAttribute(Qt::WA_DeleteOnClose); // deleted with deleteLater, after these slots run
	const std::string id = device_id.toStdString();
	connect(dialog, &QDialog::accepted, this, [this, dialog, id] {
		backend_->answer_credential(id, dialog->value().toStdString(), dialog->remember());
	});
	connect(dialog, &QDialog::rejected, this, [this, id] { backend_->answer_credential(id, std::string(), false); });
	dialog->open();
}
