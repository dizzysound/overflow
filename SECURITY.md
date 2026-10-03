# Security policy

## Reporting a vulnerability

Please do not open a public issue. Report it privately through GitHub:
**Security > Report a vulnerability** on this repository. You'll get a reply within a week.

## What Overflow stores

- **Pairing keys** for each receiver, in `credentials.json` in the plugin's settings folder
  (`%APPDATA%\obs-studio\plugin_config\obs-overflow\` on Windows).
- **AirPlay passwords** you choose to save, encrypted for the Windows user with DPAPI in
  `settings.json`.

The helper never logs key material, even with verbose logging, and the diagnostic report redacts
anything that looks like a secret. If you find a log line or report that leaks one, that's a
security bug.

## Network exposure

The helper accepts inbound connections from receivers on UDP ports 60000-60099 (timing and audio).
Overflow is meant for a trusted local network; don't expose those ports to the internet.
