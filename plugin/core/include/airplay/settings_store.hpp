// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#pragma once

#include "airplay/settings.hpp"

#include <string>

namespace airplay {

// Loads settings from a UTF-8 path. A missing file gives defaults and no warning.
Settings load_settings_file(const std::string &path, std::string *warning);

// Saves atomically: writes path + ".tmp", then renames it over path.
bool save_settings_file(const std::string &path, const Settings &settings, std::string *error);

} // namespace airplay
