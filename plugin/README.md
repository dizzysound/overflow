# Overflow (obs-overflow)

An OBS Studio plugin that sends the program output, video and audio, to AirPlay receivers
(Apple TV, Roku) on several displays at once. It encodes the program with its own H.264 encoder
(NVENC, then AMD AMF, then Intel Quick Sync, then x264) and hands video and audio to
`overflow-helper.exe`, a separate program that runs the AirPlay sessions. Your stream and
recording are not affected; if the helper fails, OBS keeps running and the plugin restarts it.

License: GPLv3 or later (see `LICENSE.txt` and `licenses/`). `eld-encoder.exe` is a separate
program under the Fraunhofer FDK AAC license (`eld-encoder-FDK-AAC-NOTICE.txt`).

## Requirements

- Windows 10 or 11, 64-bit, OBS Studio 32.x.
- The PC and the receivers on the same network (or add receivers by IP address).

## Install

1. Close OBS.
2. Unzip `obs-overflow-<version>-windows-x64.zip` into `C:\ProgramData\obs-studio\plugins\`,
   so that `C:\ProgramData\obs-studio\plugins\obs-overflow\bin\64bit\obs-overflow.dll` exists.
   (Writing there needs an administrator account.)
3. Start OBS. Open **Docks > Overflow**.

Uninstall: close OBS and delete `C:\ProgramData\obs-studio\plugins\obs-overflow`.

### Upgrading from obs-airplay

Overflow was called obs-airplay. To upgrade:

1. Close OBS and delete `C:\ProgramData\obs-studio\plugins\obs-airplay`. If both are installed,
   both drive the same TVs; the Overflow dock warns about it until the old one is gone.
2. Install Overflow as above. On its first start it copies the displays, settings and pairings from
   `plugin_config\obs-airplay` into `plugin_config\obs-overflow` and leaves the old folder alone.
3. Windows QoS policies and firewall rules that name `airplay-helper.exe` no longer match; recreate
   them for `overflow-helper.exe` (see `docs/protocol.md` and Network below).
4. OBS saves dock positions by ID, and the dock's ID changed, so it may come back hidden or
   floating. Open it from **Docks > Overflow** and drag it where it was.

## Use

- Check the displays you want and press **Start**. Give displays a location (for example
  "Friendship Hall") in **Display settings** to group them; a location's checkbox selects or
  deselects all of its displays. The first time, an Apple TV or Roku shows a
  code on screen; type it into the prompt. Pairing is remembered.
- A green light means live; yellow is connecting, waiting for a code, or retrying; red means
  it stopped (hover for the reason). "no audio" means the receiver takes only AAC-ELD audio
  and eld-encoder is off or missing.
- Right-click a display for **Restart** (a TV woken from standby with sound but no picture),
  **Reconnect**, **Display settings**, **Forget pairing** (clears the stored code and password
  only), and **Remove display** (deletes the display from the list entirely). The
  **Display settings...** button opens the selected display's settings too. Changes there apply
  when you click OK; a new TV delay, address, audio or Wi-Fi option reconnects that display once.
- Each display's line in the status area shows its TV delay and its audio trouble over the last
  minute: packets lost on the network (the TV asked for them again) and audio dropped late (it
  reached the sender too late for the TV delay). Lower a fixed TV delay while both stay at zero and
  you hear no glitches; raise it when they appear. Auto raises the delay itself when late video,
  late audio or unrecovered loss repeats (more than 3 of the 5 s reports in a minute).
- **Settings** has auto-start (when OBS starts, with streaming, with recording), the audio
  track, the video preset, and advanced options.
- The plugin never changes a TV's volume unless you turn on "Set the receiver volume when
  connecting" for that display.
- Press **Stop** in the Overflow dock before changing the canvas resolution or FPS in OBS's
  Video settings.

## Files

- Settings: `%APPDATA%\obs-studio\plugin_config\obs-overflow\settings.json`. Saved AirPlay
  passwords are encrypted for the Windows user with DPAPI.
- Pairings: `%APPDATA%\obs-studio\plugin_config\obs-overflow\credentials.json` (owned by
  `overflow-helper.exe`).
- Log: the OBS log (`Help > Log Files`); lines start with `[obs-overflow]`, and the helper's own
  lines with `[obs-overflow] [helper]`.

## Network

Receivers connect back to the PC on UDP ports 60000-60099 (timing and audio). If Windows
Firewall is on, allow `overflow-helper.exe` inbound on those ports for private networks. The
installer (a later release) adds that rule.

## Building

GitHub Actions builds everything (`.github/workflows/plugin.yml`): the Go helper, eld-encoder,
and the plugin against OBS 32.2.2. The plugin logic in `core/` builds and tests without OBS:

```bash
cmake -S plugin/tests -B plugin/build-tests
cmake --build plugin/build-tests
ctest --test-dir plugin/build-tests --output-on-failure
```

Add `-DAIRPLAY_UI_TESTS=ON -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"` to also run the
dock's offscreen tests.
