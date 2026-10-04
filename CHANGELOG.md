# Changelog

All notable changes to Overflow. The format follows [Keep a Changelog](https://keepachangelog.com/),
and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Apple VideoToolbox (hardware, macOS) and VAAPI (Linux) in the encoder fallback, ahead of x264.
- Per-display "Disconnect on these scenes": a display drops while a chosen scene is on program and
  reconnects on any other; streaming and recording are unaffected.
- A settings gear on each display's row in the dock (and a double-click on its status) opens its
  Display settings.
- macOS package: a universal (Apple silicon and Intel) `.pkg` installer and a `.zip` of
  `obs-overflow.plugin`, with `overflow-helper` and `eld-encoder` inside the bundle. Ad-hoc signed,
  not notarized. macOS 13 or later.
- Linux package: an x86_64 tarball in OBS's per-user plugin layout, built on Ubuntu 24.04 against
  OBS 32 from the OBS PPA.
- `eld-encoder` for macOS and Linux, so AAC-ELD-only receivers (Apple TV) get audio on every OS.
- Saved AirPlay passwords on macOS (login Keychain) and Linux (Secret Service: GNOME Keyring,
  KWallet, KeePassXC). Without a Secret Service on Linux the password is kept for the OBS session
  only, as before.

### Fixed

- macOS: entering a pairing code no longer leaves an empty "Overflow" window behind (verified
  with a reproduction and a test; not yet re-run in OBS).

## [0.1.0] - 2026-10-03

First release, Windows only.

### Added

- OBS plugin with the Overflow dock: displays grouped by location, master Start/Stop, per-display
  restart, reconnect, settings and pairing management, and a diagnostic report with secrets
  redacted.
- `overflow-helper`, an AirPlay sender (from doubletake) that runs one session per display: Apple TV
  (AirPlay 2, with pairing), Roku TVs, and legacy UxPlay-based receivers.
- Dedicated H.264 encoder with fallback NVENC, AMD AMF, Intel Quick Sync, x264.
- Per-display TV delay, with an Auto mode that raises it when late video, late audio or loss repeats.
- AAC-ELD audio through `eld-encoder` for receivers that require it.
- DSCP marking of media, and per-display Wi-Fi tolerance.
- Audio sample timeline: plugin audio is placed by sample position through a filtered clock mapping,
  which removed audible glitches on hardware receivers.

### Changed

- The project was renamed from obs-airplay to Overflow: the module is `obs-overflow` and the helper
  `overflow-helper`. Settings and pairings are copied from the old folder on first start.
