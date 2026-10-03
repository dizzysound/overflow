// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// The "Overflow" dock: master Start/Stop, the display list grouped
// under location headings (each with a select-all checkbox; displays with no
// location at the end), per-display actions, and the settings and credential
// dialogs. Talks only to DockBackend.
#pragma once

#include "dock-backend.hpp"

#include <QSize>
#include <QString>
#include <QWidget>

#include <string>

class QLabel;
class QMenu;
class QPoint;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class AirPlayDock : public QWidget {
public:
	explicit AirPlayDock(DockBackend *backend, QWidget *parent = nullptr);

	// A floating dock otherwise opens at whatever tiny size OBS last gave it;
	// this is the size a fresh float should start at, wide enough that all
	// three columns (Display, Status, Audio) are visible without resizing.
	// setMinimumSize() in the constructor is the hard floor.
	QSize sizeHint() const override { return QSize(640, 380); }

	// Rebuilds the list from the backend, keeping the selection and scroll
	// position. Skipped while a name is being edited.
	void refresh();
	// Opens a non-modal PIN or password prompt; the answer goes to the backend.
	void prompt_credential(const QString &device_id, const QString &display_name, const QString &kind, bool retry);
	// Prompts for a save location (default name and folder as specified for
	// the Advanced section's button) and writes backend_->diagnostics_report()
	// there. Shared by the Global settings dialog's button and the Tools
	// menu item, both of which run on the UI thread.
	void save_diagnostic_log();

	QTreeWidget *tree() const { return tree_; }
	QPushButton *start_stop_button() const { return start_stop_; }
	QPushButton *display_settings_button() const { return display_settings_; }
	QLabel *status_label() const { return status_; }
	// Test-only seam: builds a display row's context menu (with its
	// confirmation dialogs wired up) without showing it, so UI tests can
	// inspect and trigger its actions directly instead of driving the
	// blocking QMenu::exec(). Caller owns the returned menu.
	QMenu *build_display_menu(const std::string &device_id);

	// Item roles: kKeyRole holds the device ID (display rows) or the location
	// (heading rows); kHeadingRole is true on heading rows.
	static constexpr int kKeyRole = Qt::UserRole;
	static constexpr int kHeadingRole = Qt::UserRole + 1;

private:
	void on_item_changed(QTreeWidgetItem *item, int column);
	void show_context_menu(const QPoint &pos);
	void open_display_settings(const std::string &device_id);
	void open_global_settings();
	void add_display();

	DockBackend *backend_;
	QLabel *status_ = nullptr;
	QPushButton *start_stop_ = nullptr;
	QPushButton *display_settings_ = nullptr; // enabled while a display row is selected
	QTreeWidget *tree_ = nullptr;
	QLabel *hint_ = nullptr;
	bool refreshing_ = false;
};
