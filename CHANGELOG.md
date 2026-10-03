# Changelog

All notable changes to Overflow. The format follows [Keep a Changelog](https://keepachangelog.com/),
and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- OBS plugin with the Overflow dock: displays grouped by location, master Start/Stop, per-display
  restart, reconnect, settings and pairing management, and a diagnostic report with secrets
  redacted.
- `overflow-helper`, an AirPlay sender (from doubletake) that runs one session per display: Apple TV
  (AirPlay 2, with pairing), Roku TVs, and legacy UxPlay-based receivers.
- Dedicated H.264 encoder with fallback NVENC, AMD AMF, Intel Quick Sync, Apple VideoToolbox
  (hardware), VAAPI, x264.
- Per-display TV delay, with an Auto mode that raises it when late video, late audio or loss repeats.
- AAC-ELD audio through `eld-encoder` for receivers that require it.
- DSCP marking of media, and per-display Wi-Fi tolerance.
- Audio sample timeline: plugin audio is placed by sample position through a filtered clock mapping,
  which removed audible glitches on hardware receivers.
- Per-display "Disconnect on these scenes": a display drops while a chosen scene is on program and
  reconnects on any other; streaming and recording are unaffected.
- A settings gear on each display's row in the dock (and a double-click on its status) opens its
  Display settings.

### Changed

- The project was renamed from obs-airplay to Overflow: the module is `obs-overflow` and the helper
  `overflow-helper`. Settings and pairings are copied from the old folder on first start.
