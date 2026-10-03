// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/settings_store.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace airplay {

Settings load_settings_file(const std::string &path, std::string *warning)
{
	const std::filesystem::path p = std::filesystem::u8path(path);
	std::error_code ec;
	if (!std::filesystem::exists(p, ec))
		return Settings{};
	std::ifstream in(p, std::ios::binary);
	if (!in) {
		if (warning)
			*warning = "could not read " + path + "; using defaults";
		return Settings{};
	}
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();

	// Minor 5: without this, an unparsable settings.json is silently replaced
	// by defaults at the next save, and every display's name, location and
	// settings are gone with no trace. Move the bad file aside first so it
	// can be recovered; never throw, just fall back to defaults either way.
	const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
	if (parsed.is_discarded() || !parsed.is_object()) {
		const std::filesystem::path bad = std::filesystem::u8path(path + ".bad");
		std::error_code remove_ec;
		std::filesystem::remove(bad, remove_ec); // replace any older .bad
		std::error_code rename_ec;
		std::filesystem::rename(p, bad, rename_ec);
		if (warning) {
			*warning = "Overflow settings are not valid JSON; using defaults";
			if (rename_ec)
				*warning += " (could not move it aside: " + rename_ec.message() + ")";
			else
				*warning += " (moved the unreadable file to " + path + ".bad)";
		}
		return Settings{};
	}
	return settings_from_json(text, warning);
}

bool save_settings_file(const std::string &path, const Settings &settings, std::string *error)
{
	const std::filesystem::path p = std::filesystem::u8path(path);
	const std::filesystem::path tmp = std::filesystem::u8path(path + ".tmp");
	std::error_code ec;
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) {
			if (error)
				*error = "could not write " + path + ".tmp";
			return false;
		}
		const std::string text = settings_to_json(settings);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		if (!out) {
			if (error)
				*error = "could not write " + path + ".tmp";
			out.close();
			std::filesystem::remove(tmp, ec);
			return false;
		}
	}
	std::filesystem::rename(tmp, p, ec); // replaces an existing file (MoveFileExW on Windows)
	if (ec) {
		if (error)
			*error = "could not replace " + path + ": " + ec.message();
		std::filesystem::remove(tmp, ec);
		return false;
	}
	return true;
}

} // namespace airplay
