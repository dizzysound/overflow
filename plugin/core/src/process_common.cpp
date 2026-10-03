// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/process.hpp"

namespace airplay {

std::string quote_windows_arg(const std::string &arg)
{
	if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
		return arg;
	std::string out = "\"";
	for (size_t i = 0;; ++i) {
		size_t backslashes = 0;
		while (i < arg.size() && arg[i] == '\\') {
			++backslashes;
			++i;
		}
		if (i == arg.size()) {
			// Double trailing backslashes so the closing quote stays a quote.
			out.append(backslashes * 2, '\\');
			break;
		}
		if (arg[i] == '"') {
			out.append(backslashes * 2 + 1, '\\');
			out.push_back('"');
		} else {
			out.append(backslashes, '\\');
			out.push_back(arg[i]);
		}
	}
	out.push_back('"');
	return out;
}

std::string windows_command_line(const std::string &exe, const std::vector<std::string> &args)
{
	std::string line = quote_windows_arg(exe);
	for (const std::string &a : args) {
		line.push_back(' ');
		line += quote_windows_arg(a);
	}
	return line;
}

} // namespace airplay
