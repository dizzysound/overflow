// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/config_migration.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace airplay;
namespace fs = std::filesystem;

namespace {
const std::vector<std::string> kFiles = {"settings.json", "credentials.json"};

fs::path config_root()
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	auto dir = fs::temp_directory_path() / ("airplay-migration-" + std::to_string(stamp));
	fs::create_directories(dir);
	return dir;
}

void write(const fs::path &p, const std::string &text)
{
	fs::create_directories(p.parent_path());
	std::ofstream(p, std::ios::binary) << text;
}

std::string read(const fs::path &p)
{
	std::ifstream in(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("legacy_config_dir is the sibling folder, with or without a trailing separator")
{
	const fs::path root = fs::u8path("/cfg/plugin_config");
	const std::string expected = (root / "obs-airplay").u8string();
	CHECK(legacy_config_dir((root / "obs-overflow").u8string() + "/", "obs-airplay") == expected);
	CHECK(legacy_config_dir((root / "obs-overflow").u8string(), "obs-airplay") == expected);
}

TEST_CASE("first launch copies settings and pairings from the old folder and leaves the old ones")
{
	const fs::path root = config_root();
	write(root / "obs-airplay" / "settings.json", R"({"displays":[]})");
	write(root / "obs-airplay" / "credentials.json", R"({"pairings":1})");

	const MigrationResult r =
		migrate_legacy_config((root / "obs-airplay").u8string(), (root / "obs-overflow").u8string(), kFiles);

	CHECK(r.error.empty());
	CHECK(r.copied == kFiles);
	CHECK(read(root / "obs-overflow" / "settings.json") == R"({"displays":[]})");
	CHECK(read(root / "obs-overflow" / "credentials.json") == R"({"pairings":1})");
	CHECK(fs::exists(root / "obs-airplay" / "settings.json"));
	fs::remove_all(root);
}

TEST_CASE("a missing credentials file is skipped, not an error")
{
	const fs::path root = config_root();
	write(root / "obs-airplay" / "settings.json", "{}");

	const MigrationResult r =
		migrate_legacy_config((root / "obs-airplay").u8string(), (root / "obs-overflow").u8string(), kFiles);

	CHECK(r.error.empty());
	CHECK(r.copied == std::vector<std::string>{"settings.json"});
	fs::remove_all(root);
}

TEST_CASE("nothing is copied once the new folder has any of the files")
{
	const fs::path root = config_root();
	write(root / "obs-airplay" / "settings.json", "old");
	write(root / "obs-airplay" / "credentials.json", "old");
	write(root / "obs-overflow" / "credentials.json", "new");

	const MigrationResult r =
		migrate_legacy_config((root / "obs-airplay").u8string(), (root / "obs-overflow").u8string(), kFiles);

	CHECK(r.error.empty());
	CHECK(r.copied.empty());
	CHECK_FALSE(fs::exists(root / "obs-overflow" / "settings.json"));
	CHECK(read(root / "obs-overflow" / "credentials.json") == "new");
	fs::remove_all(root);
}

TEST_CASE("nothing happens without an old settings file")
{
	const fs::path root = config_root();
	write(root / "obs-airplay" / "credentials.json", "orphan");

	const MigrationResult r =
		migrate_legacy_config((root / "obs-airplay").u8string(), (root / "obs-overflow").u8string(), kFiles);

	CHECK(r.error.empty());
	CHECK(r.copied.empty());
	CHECK_FALSE(fs::exists(root / "obs-overflow" / "credentials.json"));
	fs::remove_all(root);
}
