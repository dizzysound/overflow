// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/events.hpp"
#include "airplay/frame.hpp"
#include "airplay/process.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <nlohmann/json.hpp>

using namespace airplay;

namespace {

struct Captured {
	Lines out;
	Lines err;
	std::atomic<size_t> err_bytes{0};
	std::atomic<int> exit_code{-100};
	LineSplitter out_split;
	LineSplitter err_split;

	ProcessCallbacks callbacks()
	{
		ProcessCallbacks cb;
		cb.on_stdout = [this](const char *d, size_t n) {
			out_split.feed(d, n, [this](const std::string &l) { out.add(l); });
		};
		cb.on_stderr = [this](const char *d, size_t n) {
			err_bytes += n;
			err_split.feed(d, n, [this](const std::string &l) { err.add(l); });
		};
		cb.on_exit = [this](int code) { exit_code = code; };
		return cb;
	}
};

std::vector<uint8_t> hello_frame()
{
	std::vector<uint8_t> f;
	append_frame(f, MsgType::Hello, hello_json());
	return f;
}

} // namespace

TEST_CASE("windows command-line quoting")
{
	CHECK(quote_windows_arg("abc") == "abc");
	CHECK(quote_windows_arg("") == "\"\"");
	CHECK(quote_windows_arg("a b") == "\"a b\"");
	CHECK(quote_windows_arg("a\"b") == "\"a\\\"b\"");
	CHECK(quote_windows_arg("C:\\my dir\\") == "\"C:\\my dir\\\\\"");
	CHECK(quote_windows_arg("a\\\"b") == "\"a\\\\\\\"b\"");
	CHECK(quote_windows_arg("C:\\x\\y") == "C:\\x\\y");
	CHECK(windows_command_line("C:\\Program Files\\h.exe", {"-fps", "60"}) == "\"C:\\Program Files\\h.exe\" -fps 60");
}

TEST_CASE("process: hello round trip, then exit 0 when stdin closes")
{
	Captured c;
	std::string error;
	auto p = spawn_process(STUB_HELPER_PATH, {}, c.callbacks(), &error);
	REQUIRE_MESSAGE(p, error);
	const auto f = hello_frame();
	REQUIRE(p->write_all(f.data(), f.size()));
	CHECK(wait_until([&] { return c.out.any_contains("\"stub_hello\""); }));
	CHECK(c.out.any_contains("\"ok\":true"));
	p->close_stdin();
	CHECK(p->wait_for_exit(std::chrono::seconds(5)));
	CHECK(p->exit_code() == 0);
	CHECK(c.exit_code.load() == 0);
	CHECK(c.err.any_contains("stub: stdin closed"));
}

TEST_CASE("process: arguments with spaces, quotes and backslashes arrive intact")
{
	Captured c;
	std::string error;
	const std::vector<std::string> args{"a b", "c\"d", "e\\", "C:\\dir with space\\", "", "a\tb"};
	auto p = spawn_process(STUB_HELPER_PATH, args, c.callbacks(), &error);
	REQUIRE_MESSAGE(p, error);
	REQUIRE(wait_until([&] { return c.out.any_contains("\"stub_args\""); }));
	for (const std::string &line : c.out.snapshot()) {
		const auto j = nlohmann::json::parse(line, nullptr, false);
		if (j.is_object() && j.value("event", "") == "stub_args")
			CHECK(j["args"] == nlohmann::json(args));
	}
	p->close_stdin();
	CHECK(p->wait_for_exit(std::chrono::seconds(5)));
}

TEST_CASE("process: stderr is drained while the child floods it")
{
	Captured c;
	std::string error;
	auto p = spawn_process(STUB_HELPER_PATH, {"--stderr-flood", "2048"}, c.callbacks(), &error);
	REQUIRE_MESSAGE(p, error);
	// Without a stderr reader the child blocks after about 64 KiB and never prints ready.
	CHECK(wait_until([&] { return c.out.any_contains("\"ready\""); }));
	CHECK(wait_until([&] { return c.err_bytes.load() >= 2048u * 1024u; }));
	p->close_stdin();
	CHECK(p->wait_for_exit(std::chrono::seconds(5)));
}

TEST_CASE("process: kill ends a hung child")
{
	Captured c;
	std::string error;
	auto p = spawn_process(STUB_HELPER_PATH, {"--hang-after-hello"}, c.callbacks(), &error);
	REQUIRE_MESSAGE(p, error);
	const auto f = hello_frame();
	REQUIRE(p->write_all(f.data(), f.size()));
	REQUIRE(wait_until([&] { return c.out.any_contains("\"stub_hello\""); }));
	CHECK_FALSE(p->wait_for_exit(std::chrono::milliseconds(200)));
	p->kill();
	CHECK(p->wait_for_exit(std::chrono::seconds(5)));
	// 137 (128 + SIGKILL on POSIX; the Job Object's forced exit code on
	// Windows) is the one "killed" convention on both platforms.
	CHECK(p->exit_code() == 137);
}

TEST_CASE("process: destroying a running child kills it")
{
	Captured c;
	std::string error;
	{
		auto p = spawn_process(STUB_HELPER_PATH, {"--hang-after-hello"}, c.callbacks(), &error);
		REQUIRE_MESSAGE(p, error);
		const auto f = hello_frame();
		REQUIRE(p->write_all(f.data(), f.size()));
		REQUIRE(wait_until([&] { return c.out.any_contains("\"stub_hello\""); }));
	}
	CHECK(c.exit_code.load() != -100); // on_exit ran before the destructor returned
}

TEST_CASE("process: writing to a child that exited fails instead of crashing")
{
	Captured c;
	std::string error;
	auto p = spawn_process(STUB_HELPER_PATH, {"--exit-after-hello", "3"}, c.callbacks(), &error);
	REQUIRE_MESSAGE(p, error);
	const auto f = hello_frame();
	REQUIRE(p->write_all(f.data(), f.size()));
	REQUIRE(p->wait_for_exit(std::chrono::seconds(5)));
	CHECK(p->exit_code() == 3);
	const std::vector<uint8_t> more(1 << 20, 0);
	CHECK_FALSE(p->write_all(more.data(), more.size()));
}

TEST_CASE("process: a missing executable is an error, not a crash")
{
	Captured c;
	std::string error;
	auto p = spawn_process("/nonexistent/overflow-helper", {}, c.callbacks(), &error);
	CHECK_FALSE(p);
	CHECK_FALSE(error.empty());
}
