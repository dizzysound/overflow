# Overflow (obs-overflow)

An OBS Studio plugin that sends the program output, video and audio, to AirPlay receivers
(Apple TV, Roku) on several displays at once. It encodes the program with its own H.264 encoder
(NVENC, AMD AMF, Intel Quick Sync, VideoToolbox on a Mac, VAAPI on Linux, then x264) and hands
video and audio to `overflow-helper`, a separate program that runs the AirPlay sessions. Your stream and
recording are not affected; if the helper fails, OBS keeps running and the plugin restarts it.

License: GPLv3 or later (see `LICENSE.txt` and `licenses/`). `eld-encoder` is a separate program
under the Fraunhofer FDK AAC license (`eld-encoder-FDK-AAC-NOTICE.txt`).

## Requirements

- OBS Studio 32.x on Windows 10 or 11 (64-bit), macOS 13 or later (Apple silicon or Intel), or
  Linux x86_64.
- The computer and the receivers on the same network (or add receivers by IP address).

What has been tested where:

| | Windows | macOS | Linux |
|---|---|---|---|
| Core and helper tests | yes | yes | yes (Ubuntu 24.04) |
| Loads in OBS 32 | yes | yes (32.2.2) | yes (32.2.0 from the OBS PPA, headless) |
| Real receivers | Apple TV HD, Roku TV, UxPlay panels, in production use | Apple TV 4K: one 0.2.1 session of about 6.5 min (OBS 32.2.2, VideoToolbox); picture and sound delivered | Apple TV 4K from a low-power headless host (0.2.1, test signal): picture and sound delivered; the host rendered about 11 fps, so full-frame-rate video is untested |
| Saved passwords | DPAPI | Keychain (tested against the real Keychain, not yet with a receiver) | Secret Service (tested against GNOME Keyring, not yet with a receiver) |

## Install

### Windows

The zip holds one folder, `obs-overflow`, with `bin\64bit\` (the plugin, `overflow-helper.exe` and
`eld-encoder.exe`) and `data\` inside. Where it goes depends on how OBS is installed.

**Standard install** (the OBS installer): extract the zip into OBS's plugins folder for all users,
`%ProgramData%\obs-studio\plugins` (normally `C:\ProgramData\obs-studio\plugins`; create it if it
doesn't exist). When it's right, this file exists:

    C:\ProgramData\obs-studio\plugins\obs-overflow\bin\64bit\obs-overflow.dll

1. Close OBS.
2. In PowerShell as administrator:

   ```powershell
   Expand-Archive obs-overflow-<version>-windows-x64.zip "$env:ProgramData\obs-studio\plugins"
   ```

   Or use File Explorer. `C:\ProgramData` is hidden, so type the path, and Windows asks for
   administrator approval. **Extract All** adds a folder named after the zip
   (`...\plugins\obs-overflow-<version>-windows-x64\obs-overflow\...`); remove that part of the
   path, or OBS won't find the plugin.
3. Start OBS and open **Docks > Overflow**.

**Portable OBS** (started with `--portable`, or with a `portable_mode.txt` file in the OBS folder):
OBS ignores `%ProgramData%`. Put the plugin in OBS's own folders instead:

- the contents of `obs-overflow\bin\64bit\` into `<OBS folder>\obs-plugins\64bit\`
- the contents of `obs-overflow\data\` into `<OBS folder>\data\obs-plugins\obs-overflow\`

This layout also works for a standard install (in `C:\Program Files\obs-studio`), but it mixes the
plugin with OBS's own files, so prefer `%ProgramData%` there.

**Your own folder:** OBS also loads plugins from the folders named by the `OBS_PLUGINS_PATH`
(binaries) and `OBS_PLUGINS_DATA_PATH` (data) environment variables.

To check, open **Help > Log Files > View Current Log** and look for `[obs-overflow] plugin loaded`.

Uninstall: close OBS and delete the `obs-overflow` folder you installed. For portable OBS, delete the
files you copied from the zip's `bin\64bit\` out of `obs-plugins\64bit\`, and the
`data\obs-plugins\obs-overflow\` folder.

### macOS

The package is universal (Apple silicon and Intel). It is ad-hoc signed but not notarized, so
macOS asks before running it the first time.

1. Close OBS.
2. Open `obs-overflow-<version>-macos-universal.pkg`. When macOS says it can't verify it, open
   **System Settings > Privacy & Security**, click **Open Anyway**, and run the installer. It
   installs for your user only, into `~/Library/Application Support/obs-studio/plugins/`, the folder
   OBS searches for plugins.

   Or, for your user only: unzip `obs-overflow-<version>-macos-universal.zip`, clear the download
   quarantine, and move the bundle into place:

   ```bash
   xattr -dr com.apple.quarantine obs-overflow.plugin
   mv obs-overflow.plugin ~/Library/Application\ Support/obs-studio/plugins/
   ```

3. Start OBS. Open **Docks > Overflow**. If macOS asks whether OBS may find devices on your local
   network, allow it; without it no receivers are found.

Uninstall: close OBS and delete `obs-overflow.plugin` from whichever plugins folder it is in.

### Linux

The package is built on Ubuntu 24.04 against OBS 32 from the OBS PPA (`ppa:obsproject/obs-studio`).
Other distributions with OBS 32, Qt 6.4 or later and glibc 2.39 or later should work but are untested,
as is the OBS Flatpak.

1. Close OBS.
2. Extract the tarball into your OBS plugins folder:

   ```bash
   mkdir -p ~/.config/obs-studio/plugins
   tar -xzf obs-overflow-<version>-linux-x86_64.tar.gz -C ~/.config/obs-studio/plugins
   ```

   For the OBS Flatpak use `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins` instead.
3. Start OBS. Open **Docks > Overflow**.

Hardware encoding on Linux uses whatever OBS offers: NVENC, Quick Sync or VAAPI, otherwise x264.
On Intel, VAAPI needs `intel-media-va-driver-non-free`. The version in Ubuntu 24.04 predates some
recent GPUs (Twin Lake, for example), and those get x264 unless a newer driver is installed. The
OBS log line `FFmpeg VAAPI H264 encoding supported` shows that VAAPI is available.

Uninstall: close OBS and delete `~/.config/obs-studio/plugins/obs-overflow`.

### Upgrading from obs-airplay

Overflow was called obs-airplay. To upgrade:

1. Close OBS and delete the old `obs-airplay` plugin folder (for example
   `C:\ProgramData\obs-studio\plugins\obs-airplay`). If both are installed,
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
  gear at the end of each display's row, the **Display settings...** button, or double-clicking a
  display's status opens its settings too.
  Changes there apply when you click OK; a new TV delay, address, audio or Wi-Fi option reconnects
  that display once.
- **Disconnect on these scenes** in Display settings stops AirPlay to that display while one of
  the checked scenes is on program, for example a shot that shows the TV itself. Streaming and
  recording carry on; the display reads "off on this scene" and reconnects when you switch to any
  other scene, after a short reconnect. Scenes are matched by name, so check a renamed scene again.
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

The plugin's folder is `plugin_config/obs-overflow` inside OBS's configuration folder:
`%APPDATA%\obs-studio` on Windows, `~/Library/Application Support/obs-studio` on macOS, and
`~/.config/obs-studio` on Linux (`~/.var/app/com.obsproject.Studio/config/obs-studio` for the
Flatpak).

- Settings: `settings.json`.
- Saved AirPlay passwords: on Windows, encrypted in `settings.json` for the Windows user with
  DPAPI. On macOS, in the login Keychain (items named "Overflow display password"). On Linux, in
  the Secret Service (GNOME Keyring, KWallet or KeePassXC) through libsecret; if there is no Secret
  Service, the dock does not offer to remember a password and you type it once per OBS session.
- Pairings: `credentials.json` (owned by `overflow-helper`).
- Log: the OBS log (`Help > Log Files`); lines start with `[obs-overflow]`, and the helper's own
  lines with `[obs-overflow] [helper]`.

## Network

Receivers connect back to the computer on UDP ports 60000-60099 (timing and audio).

- Windows: if Windows Firewall is on, allow `overflow-helper.exe` inbound on those ports for
  private networks. The installer (a later release) adds that rule.
- macOS: if the macOS firewall is on, allow `overflow-helper` when asked (or add it under
  **System Settings > Network > Firewall > Options**).
- Linux: if a firewall is on (ufw, firewalld), allow UDP 60000-60099 inbound from the receivers'
  network.

## Building

The plugin logic in `core/` builds and tests without OBS:

```bash
cmake -S plugin/tests -B plugin/build-tests
cmake --build plugin/build-tests
ctest --test-dir plugin/build-tests --output-on-failure
```

Add `-DAIRPLAY_UI_TESTS=ON -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"` to also run the
dock's offscreen tests.

Packages:

- macOS: `plugin/tools/package-macos.sh` (Xcode with the macOS 26.5 SDK or later, Go, CMake) builds
  the universal helper and eld-encoder, the plugin bundle, the `.pkg` and the `.zip` in
  `plugin/release/`.
- Linux: `plugin/tools/package-linux.sh` on Ubuntu 24.04 with the OBS PPA, the packages listed in
  `.github/workflows/plugin.yml`, and Go builds the tarball.
- Windows: GitHub Actions (`.github/workflows/plugin.yml`), or on a Mac
  `plugin/tools/build-windows-local.sh` (clang-cl cross-build).

GitHub Actions builds all three packages against OBS 32.2.2.
