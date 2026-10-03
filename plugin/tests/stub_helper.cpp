// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// A stand-in for overflow-helper in the plugin's tests. It speaks the stdin
// framing of docs/protocol.md, prints `ready` at startup like the real helper,
// and reports each message it receives as a JSON line on stdout.
//   --stderr-flood KB     write KB KiB to stderr before printing ready
//   --exit-after-hello N  exit with status N right after reading hello
//   --hang-after-hello    stop reading stdin after hello and never exit
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void emit(const nlohmann::json &j)
{
	const std::string line = j.dump() + "\n";
	std::fwrite(line.data(), 1, line.size(), stdout);
	std::fflush(stdout);
}

bool read_exact(uint8_t *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		const size_t r = std::fread(buf + got, 1, n - got, stdin);
		if (r == 0)
			return false;
		got += r;
	}
	return true;
}

uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; --i)
		v = (v << 8) | p[i];
	return v;
}

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	long flood_kb = 0;
	int exit_after_hello = -1;
	bool hang = false;
	nlohmann::json args = nlohmann::json::array();
	for (int i = 1; i < argc; ++i) {
		args.push_back(argv[i]);
		if (!std::strcmp(argv[i], "--stderr-flood") && i + 1 < argc)
			flood_kb = std::atol(argv[i + 1]);
		else if (!std::strcmp(argv[i], "--exit-after-hello") && i + 1 < argc)
			exit_after_hello = std::atoi(argv[i + 1]);
		else if (!std::strcmp(argv[i], "--hang-after-hello"))
			hang = true;
	}

	std::fprintf(stderr, "stub: started\n");
	std::fflush(stderr);
	if (flood_kb > 0) {
		std::string line(1023, 'x');
		line.push_back('\n');
		for (long i = 0; i < flood_kb; ++i)
			std::fwrite(line.data(), 1, line.size(), stderr);
		std::fflush(stderr);
	}
	emit({{"event", "stub_args"}, {"args", args}});
	emit({{"event", "ready"}, {"version", 1}});

	for (;;) {
		uint8_t len_buf[4];
		if (!read_exact(len_buf, 4)) {
			std::fprintf(stderr, "stub: stdin closed\n");
			return 0;
		}
		const uint32_t len = uint32_t(len_buf[0]) | (uint32_t(len_buf[1]) << 8) | (uint32_t(len_buf[2]) << 16) |
				     (uint32_t(len_buf[3]) << 24);
		if (len == 0 || len > (16u << 20)) {
			emit({{"event", "fatal"}, {"error", "bad frame length"}});
			return 1;
		}
		std::vector<uint8_t> msg(len);
		if (!read_exact(msg.data(), len))
			return 1;
		const uint8_t type = msg[0];
		const std::string payload(reinterpret_cast<const char *>(msg.data() + 1), len - 1);

		if (type == 0x01) {
			const auto hello = nlohmann::json::parse(payload, nullptr, false);
			const bool ok = hello.is_object() && hello.contains("version") && hello["version"] == 1;
			emit({{"event", "stub_hello"}, {"ok", ok}});
			if (exit_after_hello >= 0)
				return exit_after_hello;
			if (hang)
				for (;;)
					std::this_thread::sleep_for(std::chrono::seconds(1));
		} else if (type == 0x02 || type == 0x03) {
			if (len < 1 + 17)
				return 1;
			const uint64_t capture = le64(msg.data() + 1);
			const uint64_t send = le64(msg.data() + 9);
			const bool keyframe = (msg[17] & 1) != 0;
			const bool age_ok = capture != 0 && send >= capture && send - capture <= 30000000000ull;
			emit({{"event", "stub_media"},
			      {"type", type},
			      {"bytes", len - 18},
			      {"keyframe", keyframe},
			      {"age_ok", age_ok}});
		} else if (type == 0x10) {
			const auto cmd = nlohmann::json::parse(payload, nullptr, false);
			std::string name;
			if (cmd.is_object() && cmd.contains("cmd") && cmd["cmd"].is_string())
				name = cmd["cmd"].get<std::string>();
			emit({{"event", "stub_command"}, {"cmd", name}, {"raw", payload}});
			if (name == "shutdown") {
				std::fprintf(stderr, "stub: shutdown command\n");
				return 0;
			}
		}
	}
}
