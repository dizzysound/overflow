// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#pragma once

#include <string>
#include <vector>

namespace airplay {

// The plugin was renamed from obs-airplay to obs-overflow, so OBS gives it a new
// plugin_config folder. Returns the old folder beside new_dir (a trailing
// separator on new_dir is allowed).
std::string legacy_config_dir(const std::string &new_dir, const std::string &legacy_name);

struct MigrationResult {
	std::vector<std::string> copied; // file names copied into new_dir
	std::string error;               // empty when nothing went wrong
};

// Copies files from old_dir into new_dir, once: only when new_dir has none of
// them and old_dir has settings.json. Copies rather than moves, so the old
// plugin still works if it is reinstalled. Paths are UTF-8.
MigrationResult migrate_legacy_config(const std::string &old_dir, const std::string &new_dir,
				      const std::vector<std::string> &files);

} // namespace airplay
