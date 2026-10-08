# Plugin <-> helper protocol, version 1

The OBS plugin launches `overflow-helper` and talks to it over the child's standard
streams. There are no sockets and no ports.

- **stdin (plugin -> helper):** binary frames.
- **stdout (helper -> plugin):** UTF-8 JSON, one object per line.
- **stderr (helper -> plugin):** free-form log text. The plugin copies it into the OBS log.

When stdin reaches EOF, the helper disconnects every room and exits with status 0.

## Revision 1.1

Revision 1.1 adds optional fields and commands. `hello` and `ready` still carry version 1,
so there is no field a plugin can check to tell a 1.0 helper from a 1.1 one. This does **not**
degrade cleanly: a 1.0 helper does not know `set_displays`, so it logs the command as unknown
and selects nothing, and it emits `room` events, not `display`. A plugin that sends only
`set_displays` and parses only `display` sees every display sit idle, with no error, against a
1.0 helper. **The plugin ships and launches its own matching helper binary**, so this mismatch
should only arise from a partial update or a hand-copied `overflow-helper.exe` on site. A client
that must drive a 1.0 helper sends `set_rooms` and accepts `room`, the revision 1.0 names, which
a 1.1 helper also still accepts and emits as aliases. Additions:

- `volume_db` in `set_rooms` entries, and the `set_volume` command.
- `audio` and `audio_reason` in `live` room events.
- `audio` (a boolean) in `set_displays`/`set_rooms` entries, turning a display's audio off
  (video only).
- `wifi_tolerant` (a boolean) in `set_displays`/`set_rooms` entries, letting a display's relay
  ride out brief network stalls.
- The `restart` command.
- `ip` and `port` in `set_rooms` entries (manual receiver addresses).
- Device IDs are case-insensitive: `set_rooms` entries, every command that carries a
  `device_id`, and discovery (mDNS reports some receivers, e.g. the Android UxPlay port, in
  lowercase) all resolve to the same room regardless of case. **The helper uppercases every
  `device_id` it receives, and reports every `device_id` in uppercase on every event** —
  `devices`, `display`, and any other event that carries one. A client must either send
  uppercase `device_id` values, or match its own records against the uppercased form the helper
  sends back; comparing an event's `device_id` case-sensitively against a lowercase or
  mixed-case value the client invented (for example a manual display's own ID) will not match.
- The `-discovery` launch flag (native DNS-SD discovery on Windows).
- Canonical names: `set_displays` (array key `displays`) and `display` events. `set_rooms` (key
  `rooms`) is still accepted; the helper emits only `display` events. Terminology: one display
  is one AirPlay receiver; several displays may share a room.

## Revision 1.2

Revision 1.2 adds one optional field and two events. `hello` and `ready` still carry version 1.
Everything is additive and tolerant: a 1.1 plugin ignores both events, and a 1.1 helper ignores
`latency_ms`, which leaves every display on the helper's own default lead and relay budget.
Additions:

- `latency_ms` in `set_displays`/`set_rooms` entries: that display's playout lead in
  milliseconds. Absent or `0` keeps the helper's process-wide lead (`-target-latency-ms`) and its
  67 ms relay budget (250 ms for a `wifi_tolerant` display). A positive value sets that
  display's lead and derives its relay budget from it (the lead minus 30 ms, never below
  67 ms). The helper reads it when it connects the display.
- The `keyframe` event, `{"event":"keyframe","device_id":"...","reason":"..."}`: the helper asks
  the plugin to make its encoder emit an IDR. `reason` is `backlog` (a display's relay queue
  shed frames and needs the next IDR to resume), `join` (a display just joined the shared
  encoder and would otherwise wait up to a full GOP) or `receiver` (the receiver itself sent a
  `forceKeyFrame` request). `device_id` names the display that caused the request. One
  shared encoder means one IDR serves every display, so the helper throttles this event to at
  most one per 250 ms; a request inside the window is delivered once at the window's end
  (carrying the most recent request's `device_id` and `reason`).
- The `delivery` event, `{"event":"delivery","device_id":"...","frames":N,"p50_ms":A,"p99_ms":B,"late":K,"lead_ms":L}`,
  sent every 5 s for each display that wrote video frames in that window. Each frame's age is
  OBS capture to the moment the helper finished writing it to the receiver. `frames` is the
  count in the window, `p50_ms` and `p99_ms` are percentiles of the age, `late` counts frames
  whose age exceeded the display's lead, and `lead_ms` is that lead (`0` when no per-display
  lead was set, in which case `late` is `0`). A display with no frames in the window is
  omitted.

## Revision 1.3

Revision 1.3 adds one optional field and three `delivery` event fields. `hello` and `ready` still
carry version 1. A 1.2 helper ignores the field, which leaves every display on the automatic audio
format, and a 1.2 plugin ignores the event fields.

- `audio_format` in `set_displays`/`set_rooms` entries forces that display's screen-audio codec:
  `"alac"` (ct=2) or `"aac-eld"` (ct=8). Absent, `""` or `"auto"` keeps the helper's choice from
  the receiver's `/info` (AAC-ELD when offered and an encoder is available, otherwise ALAC). A
  forced codec is sent even when the receiver does not advertise it, so it is for listening tests
  on receivers whose `/info` lists no formats; a receiver that cannot decode it plays noise or
  silence, or drops the session. A forced AAC-ELD falls back to the automatic choice, with a log
  line, when no AAC-ELD encoder is available. Any other value is ignored and logged. Applies at
  session start; for a live display, send `restart`.
- `audio_lost` and `audio_resent` in the `delivery` event: audio packets the receiver asked to be
  resent in the window (lost on the network), and how many of those were still in the sender's
  history and resent. A resend is heard only if it arrives within the display's lead, so these
  show how close a display's TV delay is to its network's loss: lower the delay while they stay at
  zero, raise it when they appear.
- `audio_dropped` in the `delivery` event: audio frames the helper dropped in the window because
  they reached it too late to play within the display's lead. Each is an audible gap the receiver
  never reports; a lead too small for this PC's audio path shows here first.

Network priority: the helper marks its AirPlay packets with DSCP classes, EF (46) for audio, audio
resends and timing, AF41 (34) for the video stream, so switches and Wi-Fi access points (WMM) queue
them ahead of bulk traffic. On the streaming PC (Windows, 2026-10-01) the helper's own marking reached
the wire. Windows can ignore a program's own marking (by default on many versions), so a
policy-based QoS rule covering the helper is the backstop, for example (elevated PowerShell):

```powershell
New-NetQosPolicy -Name "Overflow helper UDP" -AppPathNameMatchCondition "overflow-helper.exe" -IPProtocolMatchCondition UDP -DSCPAction 46 -NetworkProfile All
New-NetQosPolicy -Name "Overflow helper TCP" -AppPathNameMatchCondition "overflow-helper.exe" -IPProtocolMatchCondition TCP -DSCPAction 34 -NetworkProfile All
```

On a PC that is not joined to a domain, Windows applies these only with the string value
`Do not use NLA` = `1` under `HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\QoS`.

Policies made before the Overflow rename match `airplay-helper.exe` and no longer apply; remove
them (`Remove-NetQosPolicy -Name "AirPlay helper UDP"`, and the TCP one) and create the two above.

## Revision 1.4

Revision 1.4 lets a display's TV delay slide during a session (spec
`docs/superpowers/specs/2026-10-02-dynamic-tv-delay-design.md`). `hello` and `ready` still carry
version 1. A 1.3 helper ignores the new field and command; a 1.3 plugin ignores the new event fields.

- `ready` adds `"capabilities":["live_lead"]`. A plugin sends `set_lead` only to a helper that lists
  `live_lead`.
- `lead_headroom_ms` in `set_displays`/`set_rooms` entries (0-500, with `latency_ms`): the session
  announces `latency_ms + lead_headroom_ms` (at most 2000) as its lead at SETUP and starts at
  `latency_ms`. Absent or 0: the lead is fixed for the session, as before. Applies at session start.
- `{"cmd":"set_lead","device_id":"...","lead_ms":N}`: a new target for that display's live session,
  clamped to 40 ms (or the session's start, if lower) and the announced ceiling. The session slides
  to it at 300 ppm of elapsed time (0.3 ms per second; helper flag `-lead-slide-ppm`): video
  timestamps and the audio sync mapping move together, and the sync packets' latency field stays at
  the ceiling. Ignored, with a log line, for a display that is not live or has no headroom. Not
  remembered across a reconnect.
- `delivery` adds `lead_target_ms` and `lead_ceiling_ms`; `lead_ms` is the effective lead at the end
  of the window. For a fixed lead all three are equal.

## Revision 1.5

Revision 1.5 changes how the helper times media and adds one event field. `hello` and `ready` still carry
version 1; nothing the plugin sends changes. A 1.4 plugin ignores the new field.

- Presentation times come from a filtered clock mapping, not from each message's own `helper_now - (send_ns -
  capture_ns)`. The helper keeps the minimum of `helper_now - send_ns` over a 2 s window and slews toward it at
  200 ppm (steps of more than 20 ms apply at once; a forward step needs 250 ms of consistent evidence). On Windows
  the helper's clock moves only at the system timer tick, so per-message mapping scattered timestamps by up to a
  tick (`data/2026-09-28-quality-latency/16-rtp-gaps.md`).
- Audio `capture_ns` is also the sample position: the helper places each chunk at
  `round((capture_ns - first_capture_ns) * 44100 / 1e9)` and advances the receivers' RTP timeline by exactly the
  samples sent. A chunk up to 2 samples off is contiguous; a forward gap up to 0.5 s is filled with silence; an
  overlap is dropped; anything larger restarts the timeline. OBS's `audio_data.timestamp` is exact on the sample
  grid, so the plugin keeps sending it unchanged.
- `delivery` adds `audio_jumps`: audio frames whose RTP timestamp did not follow the previous frame's, or whose
  source clock mapping was reset, in the window. It is 0 for a healthy session; the plugin logs a warning otherwise.
  A timeline "restart" is a forward RTP jump on the same origin (receivers see no new stream), and frames dropped
  as stale also count, through the forward jump of the frame after them.

## Launch flags

| Flag | Meaning |
|---|---|
| `-creds <path>` | Pairing credentials file. Required with the default `file` backend. |
| `-cred-backend file\|keyring` | Where PIN pairings are stored (default `file`). Any other value is rejected at startup. |
| `-port-range lo-hi` | Local UDP ports for timing and audio, at least 3 ports. Use a fixed range so the installer's firewall rule can be narrow. Empty means OS-chosen ephemeral ports. |
| `-fps N` | The OBS output frame rate (1-120, default 30). The plugin sets it from the OBS video settings. The helper uses it to size each room's video queue; a room that falls behind drops frames and resumes at the next keyframe. |
| `-debug` | Verbose protocol logging to stderr. |
| `-timing auto\|ntp` | Timing protocol for mirroring (default `auto`, which negotiates PTP or NTP per receiver). `ntp` forces NTP timing for every room; it is a fallback for receivers where PTP mirroring fails (for example, the Apple TV HD). Any other value is rejected at startup. |
| `-target-latency-ms N` | Joint audio/video playout latency override, 0 to 2000 ms (default 0, the automatic AirPlay policy). The plugin passes it only when its target-latency setting is above 0. |
| `-audio-key auto\|raw` | Legacy AES audio key handling (default `auto`, hashed with the pair-verify secret). `raw` sends the unhashed FairPlay key, for old AirPlay 1 receivers that play a hashed key as noise. |
| `-video-key auto\|raw` | Legacy AES video key handling (default `auto`, hashed with the pair-verify secret). `raw` sends the unhashed FairPlay key, for old AirPlay 1 receivers. |
| `-volume keep\|DB` | Receiver volume at the start of each session (default `keep`, which never changes it). `DB` is a level from -30 to 0 dB (0 is full scale). A display's own `volume_db` (in `set_displays`/`set_volume`) overrides this per display. |
| `-device DEVICEID@IP[:PORT]` | A manual receiver address for a receiver mDNS misses, for example another VLAN or a PC where several programs share UDP 5353. Repeatable. `PORT` defaults to 7000. Unlike the `ip`/`port` fields in `set_displays`, a device named here is also listed in `devices` events. |
| `-eld-encoder <path>` | The `eld-encoder` program used for AAC-ELD audio. Optional; the default is `eld-encoder.exe` (`eld-encoder` outside Windows) in the same directory as `overflow-helper`. See [AAC-ELD audio](#aac-eld-audio). |
| `-discovery auto\|zeroconf` | How receivers are discovered (default `auto`). On Windows 10 1709 and later, `auto` browses through the Windows DNS-SD service (`dnsapi.dll`), which shares UDP 5353 cleanly with Bonjour, vMix and other mDNS software; if that API is missing or fails, the helper falls back to its built-in mDNS listener and logs one line. Elsewhere `auto` is the built-in listener. `zeroconf` forces the built-in listener. Any other value is rejected at startup. |
| `-lead-slide-ppm N` | Rate at which a live session slides its TV delay, in ppm of elapsed time (1-5000; 0 or absent means 300). For on-site tests; the plugin never passes it. |

## Process contract

**Drain both output streams.** The plugin must read stdout and stderr continuously, each on
its own thread. A stalled stdout blocks room management. A stalled stderr blocks the
helper's logging, which can stall the stream workers.

**Exit status.**

| Outcome | Meaning |
|---|---|
| Exit 0 | stdin reached EOF or the plugin sent `shutdown`. |
| Exit 1 after a `fatal` event | Protocol error (or daemon startup failure); the event says which. |
| Exit 1 with no `ready` event | Bad launch flags or startup failure. Read stderr for the reason. |

**Shutdown.** Close stdin (or send `shutdown`, then close stdin). Wait up to about 10 s for
the process to exit, then kill it. Teardown with hung receivers can be slow.

**Orphan protection.** The helper exits when stdin reaches EOF, so it dies when the plugin
does, provided no other process holds the write end of the pipe. On Windows:

- Make the parent's pipe ends non-inheritable, or the child inherits its own stdin write end
  and never sees EOF.
- As a backstop, assign the child to a Job Object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so it is killed if OBS crashes.
- Launch with `CREATE_NO_WINDOW`.
- Verify: libobs `os_process_pipe_create` is one-directional (it connects either stdin or
  stdout, not both). The plugin will likely need native process creation
  (`CreateProcess` / `posix_spawn`) with three pipes.

## AAC-ELD audio

Some receivers (the Newline Cast panel, for one) accept only AAC-ELD for screen audio. The
helper does not contain an AAC-ELD encoder. It runs a separate program, `eld-encoder`, built
from `eld-encoder/` against the Fraunhofer FDK AAC library, and exchanges PCM and encoded
frames with it over pipes (protocol in `eld-encoder/README.md`). The encoder is a separate
program because the FDK AAC license is not GPL-compatible, and the helper is GPLv3.

- **Ship `eld-encoder.exe` next to `overflow-helper.exe`.** The helper looks there by default;
  `-eld-encoder <path>` overrides the location. Build it with `make eld-encoder-windows` in
  `helper/` (separate from `make helper-windows`, because building fdk-aac is slow).
- When an AAC-ELD session is set up, the helper probes for the encoder once (start it, check
  its handshake, close it), then runs one encoder process for that session's audio. On
  Windows the encoder is launched with `CREATE_NO_WINDOW` and stays in the helper's Job
  Object (see Orphan protection). On every platform it exits on its own when its stdin
  closes. Its stderr appears in the helper's stderr, prefixed `[ELD-ENCODER]`.
- **When the encoder is absent** (or fails its startup handshake), a receiver whose only
  screen-audio codec is AAC-ELD gets video without audio. The helper never substitutes ALAC,
  which such receivers decode as noise and which can crash them. It logs one warning per
  session: `receiver only accepts AAC-ELD audio, which this build cannot encode; streaming
  video without audio`. Receivers that accept ALAC are unaffected.
- If the encoder hangs (no reply within 2 s) or exits during a session, that session's audio
  ends; video continues.

## Timestamps

- `send_ns - capture_ns` (the sample's age) must be between 0 and 30 s. A larger age makes
  the receiver-clock mapping fail and the room reconnects.
- `capture_ns` must never be zero.
- Recommended OBS sources:
  - video: `encoder_packet.sys_dts_usec * 1000` (with no B-frames, pts == dts);
  - audio: `audio_data.timestamp` (it must advance with the sample count; see revision 1.5).

## Tolerant parsing

The helper ignores, and logs to stderr, the following:

- unknown message types;
- malformed or unknown commands;
- a `credential` for a room that is not selected or not discovered, or with an empty value.

The keyframe flag bit is advisory. The helper detects IDR access units itself.

## stdin framing

```
u32 little-endian  length   = 1 + len(payload), at most 16 MiB
u8                 type
bytes              payload
```

| Type | Name | Payload |
|---|---|---|
| 0x01 | hello | JSON, see below. **Must be the first message.** |
| 0x02 | video_au | media header + one H.264 Annex-B access unit. Must contain at least 1 byte of data after the header; the helper treats an empty access unit as a fatal protocol error. |
| 0x03 | audio_pcm | media header + interleaved S16LE stereo PCM, 44100 Hz; length a multiple of 4. Must contain at least 1 byte of data after the header; the helper treats an empty payload as a fatal protocol error. |
| 0x10 | command | JSON, see below |

**media header** (17 bytes, little-endian):

```
u64 capture_ns   OBS os_gettime_ns() when the frame/first sample was captured
u64 send_ns      OBS os_gettime_ns() when the plugin wrote this message
u8  flags        bit 0 = keyframe (IDR); other bits zero
```

The helper maps `capture_ns` to its own clock through a filtered estimate of the two clocks'
offset (revision 1.5). Video and audio must use the same clock.

**Video rules:**
- H.264 only.
- No B-frames.
- Every keyframe access unit must contain SPS and PPS NAL units in-band. OBS encoders
  keep them in extra data, so the plugin prepends them.
- One message per access unit.

**hello:**
```json
{"version":1,"video":{"codec":"h264"},"audio":{"sample_rate":44100,"channels":2,"format":"s16le"}}
```

**commands:**
```json
{"cmd":"set_displays","displays":[{"device_id":"AA:BB:CC:DD:EE:FF","auto_reconnect":true,"volume_db":-12}]}
{"cmd":"set_displays","displays":[{"device_id":"AA:BB:CC:DD:EE:FF","auto_reconnect":true,"audio":false}]}
{"cmd":"set_displays","displays":[{"device_id":"0C:FB:30:58:DF:2E","auto_reconnect":true,"wifi_tolerant":true}]}
{"cmd":"set_volume","device_id":"AA:BB:CC:DD:EE:FF","volume_db":-12}
{"cmd":"set_displays","displays":[{"device_id":"0C:FB:30:58:DF:2E","auto_reconnect":true,"ip":"10.20.0.178","port":7000}]}
{"cmd":"credential","device_id":"AA:BB:CC:DD:EE:FF","value":"1234"}
{"cmd":"reconnect","device_id":"AA:BB:CC:DD:EE:FF"}
{"cmd":"restart","device_id":"AA:BB:CC:DD:EE:FF"}
{"cmd":"forget","device_id":"AA:BB:CC:DD:EE:FF"}
{"cmd":"shutdown"}
```

- `set_displays` replaces the whole selection. Displays that are dropped are disconnected. The
  plugin must always send `auto_reconnect` in each entry (omitted fields decode as false).
  `set_rooms` (array key `rooms`) is still accepted as an alias for `set_displays`; if a command
  carries both `displays` and `rooms`, `displays` wins.
- `volume_db` (optional, a JSON number from -30 to 0 dB; 0 is full scale) sets that
  receiver's volume at the start of each session, overriding the `-volume` flag. Absent means
  the helper sends no volume command for that display (unless `-volume` is set). A value
  outside the range is ignored and logged; the display is still selected. Include each
  display's current level in every `set_displays`: an entry without `volume_db` clears the
  stored level.
- `audio` (optional boolean) selects video-only streaming for that display: absent or `true`
  is today's behavior; `false` sends no audio for the session (the SETUP still negotiates an audio session, as with `-no-audio`, but no audio is ever streamed), and its `live` events carry
  `"audio":"off"` with `audio_reason` `"turned off for this display"`. The value applies at the
  start of each session; for a display that is already live, send `restart` to apply a change.
  It is stored in the same way as `volume_db`: include each display's current value in every
  `set_displays`, and reconnects keep the last value sent.
- `wifi_tolerant` (optional boolean) lets that display's relay tolerate brief network stalls:
  absent or `false` is today's behavior; `true` raises that display's frame-queue budget in the
  helper's shared relay from 67 ms to 250 ms. A display that falls further behind than its budget
  sheds its backlog and freezes until the next keyframe (up to a second), so a Wi-Fi receiver
  that stalls for more than 67 ms freezes often at the default. The trade-off is up to ~0.25 s
  extra delay on that display during a stall, until it catches up. Other displays are
  unaffected. The value applies at the start of each session; for a display that is already
  live, send `restart` to apply a change. It is stored in the same way as `audio`: include each
  display's current value in every `set_displays`, and reconnects keep the last value sent.
- `ip` and `port` (optional) address the receiver directly, for receivers discovery cannot
  see (another VLAN, or a PC where several programs share UDP 5353). With `ip` set, the display
  connects there whether or not the device is discovered, and a manual address wins over a
  discovered one; `port` defaults to 7000. `ip` must be an IP address literal (no host names).
  An invalid `ip`, a port outside 1-65535, or a `port` without `ip` is ignored and logged.
  `device_id` stays the display's key in events; the plugin can use the receiver's real device
  ID or any stable name, but the helper reports it back uppercased on every event (see Revision
  1.1 above) — compare against the uppercased form, not the spelling sent. A manual display
  never goes `offline`: when it drops it retries at the same address. A live display keeps its
  current session when `set_displays` changes its `ip`/`port`; send `restart` to move it to the
  new address. The `-device` launch flag keeps working and, unlike this field, also lists the
  receiver in `devices` events.
- `set_volume` changes a live display's volume now (SET_PARAMETER) and stores the level for
  that display's later sessions, until the next `set_displays`. For a display that is not live,
  it only stores the level (a display that is still connecting gets it at its next session). An
  out-of-range or missing `volume_db` is ignored and logged. It does nothing for a display that
  is not selected.
- `credential` answers a `credential` display state; it's the PIN or password.
- `reconnect` retries a `failed` display immediately.
- `restart` ends the display's session, whatever its state, and connects again as soon as the
  old session has closed, with the backoff reset. The display reports `connecting` until the new
  session is `live` (or prompts, or fails as usual). Offer it as a per-display Restart action: a
  TV woken from standby keeps audio but loses video until a new session sends the codec
  configuration. It does nothing for a display that is not selected.
- `forget` deletes stored pairing for a device.

## stdout events

```json
{"event":"ready","version":1,"capabilities":["live_lead"]}
{"event":"devices","devices":[{"device_id":"...","name":"Lobby","model":"AppleTV14,1","ip":"10.0.0.5","port":7000}]}
{"event":"display","device_id":"...","state":"live"}
{"event":"display","device_id":"...","state":"live","audio":"off","audio_reason":"receiver only accepts AAC-ELD audio, which this build cannot encode"}
{"event":"display","device_id":"...","state":"credential","credential_kind":"pin"}
{"event":"display","device_id":"...","state":"retrying","error":"mirror setup failed: ..."}
{"event":"keyframe","device_id":"...","reason":"join"}
{"event":"delivery","device_id":"...","frames":150,"p50_ms":38,"p99_ms":71,"late":0,"lead_ms":95,"lead_target_ms":95,"lead_ceiling_ms":95}
{"event":"fatal","error":"..."}
```

- `devices` is sent whenever the discovered set changes. The first `devices` event arrives
  only once at least one device has been discovered, so on an empty network it may never
  arrive.
- `display` is sent whenever a selected display's state, credential kind or error changes.
  `room` is retired as an emitted event name; the helper emits only `display`.
- A `live` display event carries `audio`: `"on"` while screen audio streams to the receiver, or
  `"off"` for a video-only session, with `audio_reason` saying why: the AAC-ELD case above,
  `audio capture failed: ...`, `audio stream ended: ...` (for example when `eld-encoder` stops
  mid-session), or `turned off for this display` when that display's own `audio` field is
  `false`. `receiver did not negotiate audio` covers two cases the helper cannot tell apart
  in the reason text: the receiver's SETUP response offered no audio ports, and the receiver
  offered audio ports but the helper's own audio stream setup against them failed; either way,
  the session is video-only. The first `live` event can arrive before
  the audio path has started and then lacks `audio`; another `live` event with `audio` follows
  within a tick or two. Other states never carry these fields. Suggested UI: a green light with
  a "no audio" badge and the reason as its tooltip.

**Display states:**

| State | Meaning | Suggested light |
|---|---|---|
| `idle` | Deselected | gray |
| `offline` | Selected, not discovered on the network | gray |
| `connecting` | Pairing or setup in progress | yellow |
| `credential` | Receiver wants a PIN (`credential_kind: "pin"`) or password (`"password"`) | yellow |
| `live` | Streaming | green |
| `retrying` | Dropped; auto-reconnect is scheduled | yellow |
| `failed` | Dropped with auto-reconnect off; waits for `reconnect` | red |

A FairPlay or pairing rejection from the receiver (for example an `/fp-setup` HTTP 403) goes to
`failed` immediately, even when auto-reconnect is on: repeatedly retrying this kind of rejection
has locked an Apple TV out until it was restarted. Its `error` carries the original message plus
the suffix ` (auto-reconnect paused: restart the receiver, then reconnect)`. The display stays
`failed` until a `reconnect` command, which resets the backoff as usual. The plugin should tell
the operator to restart the receiver before pressing Reconnect. Ordinary drops (connection
refused, timeouts, a wrong PIN or password, a stream error after going live) keep the normal
retry behavior.

**Credentials across reconnects.** PIN pairing is remembered by the helper, so a paired
receiver does not prompt again. A `password`-kind display prompts again on every reconnect,
because the helper does not retain passwords. The plugin should cache each device's password
and answer `password` prompts automatically.
