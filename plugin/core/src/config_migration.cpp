// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/config_migration.hpp"

#include <filesystem>
#include <system_error>

namespace airplay {

namespace fs = std::filesystem;

std::string legacy_config_dir(const std::string &new_dir, const std::string &legacy_name)
{
	fs::path p = fs::u8path(new_dir);
	if (!p.has_filename())
		p = p.parent_path(); // ".../obs-overflow/" becomes ".../obs-overflow"
	return (p.parent_path() / fs::u8path(legacy_name)).u8string();
}

MigrationResult migrate_legacy_config(const std::string &old_dir, const std::string &new_dir,
				      const std::vector<std::string> &files)
{
	MigrationResult result;
	const fs::path from = fs::u8path(old_dir);
	const fs::path to = fs::u8path(new_dir);
	std::error_code ec;

	if (!fs::exists(from / "settings.json", ec))
		return result;
	for (const std::string &name : files) {
		if (fs::exists(to / fs::u8path(name), ec))
			return result; // already set up under the new name
	}

	fs::create_directories(to, ec);
	if (ec) {
		result.error = "could not create " + new_dir + ": " + ec.message();
		return result;
	}
	for (const std::string &name : files) {
		const fs::path src = from / fs::u8path(name);
		if (!fs::exists(src, ec))
			continue;
		fs::copy_file(src, to / fs::u8path(name), fs::copy_options::none, ec);
		if (ec) {
			result.error = "could not copy " + name + " from " + old_dir + ": " + ec.message();
			return result;
		}
		result.copied.push_back(name);
	}
	return result;
}

} // namespace airplay
