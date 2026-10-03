// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Defense-in-depth scrubbing of secret-shaped text before it leaves the
// process. The helper's own debug logging is the primary control (see
// helper/internal/airplay/debug.go's keyFingerprint), but the plugin's
// "Save diagnostic log..." feature captures helper stderr verbatim into a
// log ring the operator can then share, so a second, independent guard
// runs here on anything written into the diagnostic report.
#pragma once

#include <string>

namespace airplay {

// Replaces any run of 32 or more contiguous hex characters, and any
// base64-like run of 40 or more contiguous characters (letters, digits,
// '+', '/', '='), with "<redacted>". Everything else in the line —
// timestamps, IPs, short device/MAC-style identifiers, and ordinary
// prose — is left untouched. Pure function: no I/O, no logging.
std::string redact_secrets(const std::string &line);

} // namespace airplay
