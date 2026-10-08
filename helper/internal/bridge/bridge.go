package bridge

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"sort"
	"strconv"
	"strings"
	"time"

	"doubletake/internal/airplay"
	"doubletake/internal/daemon"
)

type Config struct {
	CredFile         string
	CredBackend      string // "file" or "keyring"
	PortMin, PortMax int
	Debug            bool
	FPS              int                   // OBS output frame rate for queue accounting; default 30
	ForceNTP         bool                  // override the negotiated timing protocol to NTP (-timing ntp)
	AudioKey         airplay.LegacyKeyMode // legacy AES audio key: auto, raw or mixed (-audio-key)
	VideoKey         airplay.LegacyKeyMode // AES-CTR video key input: auto, raw or mixed (-video-key)
	SetVolume        bool                  // send VolumeDB at session start (-volume); false leaves receiver volume alone
	VolumeDB         float64               // receiver volume in dB when SetVolume
	MinimumVideoLead time.Duration
	TickInterval     time.Duration // default 250 ms
	ExtraDevices     []Device      // receivers mDNS cannot see (tests, manual IPs)
	Now              func() time.Time
}

// Run serves one plugin session: it reads stdin messages from in and writes
// events to out until in reaches EOF (nil), a shutdown command (nil), a
// protocol violation (error), or ctx cancellation (nil).
func Run(ctx context.Context, cfg Config, in io.Reader, out io.Writer) error {
	if cfg.TickInterval <= 0 {
		cfg.TickInterval = 250 * time.Millisecond
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.FPS <= 0 {
		cfg.FPS = 30
	}
	events := &eventWriter{out: out}
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()

	video := airplay.NewExternalVideoSource(90)
	audio := airplay.NewExternalAudioSource()
	broadcast := airplay.NewBroadcastCaptureWithFrameRate(video.Capture(), cfg.FPS)
	// A slow room drops frames and resyncs at the next IDR instead of being
	// detached and forced through a full reconnect.
	broadcast.SetDropToKeyframe(true)
	go func() { _ = broadcast.Run() }()

	// Each keyframe event is logged with the time the next IDR took to reach
	// the broadcast (Info; a warning past 1 s).
	kfLatency := newKeyframeLatency(time.Now, log.Printf)
	broadcast.SetIDRObserver(kfLatency.wantIDR, kfLatency.sawIDR)
	kf := newKeyframeThrottle(250*time.Millisecond, func(deviceID, reason string) {
		kfLatency.requested(deviceID, reason)
		events.keyframe(deviceID, reason)
	})
	defer kf.stop()
	// A display that keeps shedding is backed off per display; "join" and
	// "receiver" requests pass straight to the throttle.
	kfBackoff := newKeyframeBackoff(time.Now)
	requestKeyframe := func(deviceID, reason string) {
		if reason == "backlog" && !kfBackoff.allow(deviceID) {
			return
		}
		kf.request(deviceID, reason)
	}
	retryKeyframe := func(deviceID, reason string) {
		if kfBackoff.allow(deviceID) {
			kf.request(deviceID, reason)
		}
	}
	dw := newDeliveryWindows()

	d, err := daemon.New(daemon.Config{
		CredFile:    cfg.CredFile,
		CredBackend: cfg.CredBackend,
		PortMin:     cfg.PortMin,
		PortMax:     cfg.PortMax,
		Debug:       cfg.Debug,
		ForceNTP:    cfg.ForceNTP,
		AudioKey:    cfg.AudioKey,
		VideoKey:    cfg.VideoKey,
		SetVolume:   cfg.SetVolume,
		VolumeDB:    cfg.VolumeDB,
		// A streaming PC's shared UDP 5353 (Bonjour, Windows Dnscache, ScreensConnect,
		// node, vMix all bound to it) delivers mDNS replies to the helper only
		// intermittently, so the daemon's 30s default TTL flapped devices in and
		// out. Give the embedded bridge more slack between sightings.
		DeviceTTL: 2 * time.Minute,
		External: &daemon.ExternalMedia{
			Video: broadcast, Audio: audio, MinimumVideoLead: cfg.MinimumVideoLead,
			// Both callbacks run on daemon hot paths: keep them to a throttle
			// check and one emit, or one append.
			RequestKeyframe: requestKeyframe,
			RetryKeyframe:   retryKeyframe,
			FrameDelivered:  func(id string, lead airplay.LeadState, age time.Duration) { dw.add(id, lead, age) },
			AudioResend:     func(id string, requested, resent int) { dw.addResend(id, requested, resent) },
			AudioDropped:    func(id string) { dw.addDropped(id) },
			AudioJump:       func(id string, _ int) { dw.addJump(id) },
		},
	})
	if err != nil {
		video.Close()
		audio.Close()
		events.fatal(err)
		return err
	}
	daemonDone := make(chan error, 1)
	go func() { daemonDone <- d.RunEmbedded(ctx) }()
	events.ready()

	cmds := make(chan Command, 16)
	readErr := make(chan error, 1)
	go readLoop(ctx, cfg, in, video, audio, cmds, readErr)

	m := NewManager()
	var lastDevices []Device
	ticker := time.NewTicker(cfg.TickInterval)
	defer ticker.Stop()
	flush := time.NewTicker(5 * time.Second)
	defer flush.Stop()

	// d.Shutdown detaches every stream, so no per-room disconnects are needed.
	shutdown := func() {
		cancel()
		d.Shutdown()
		video.Close()
		audio.Close()
		<-daemonDone
	}

	for {
		select {
		case <-ctx.Done():
			shutdown()
			return nil
		case err := <-readErr:
			if errors.Is(err, io.EOF) {
				shutdown()
				return nil
			}
			// Report before teardown, which can be slow with hung receivers.
			events.fatal(err)
			shutdown()
			return err
		case cmd := <-cmds:
			// Device IDs are case-insensitive across all sources (mDNS,
			// -device, and operator-typed commands); normalize to uppercase
			// here so every command reaches the room set_rooms/sanitizeRooms
			// already normalized.
			if cmd.DeviceID != "" {
				cmd.DeviceID = strings.ToUpper(cmd.DeviceID)
			}
			switch cmd.Cmd {
			case "shutdown":
				shutdown()
				return nil
			case "set_displays", "set_rooms":
				m.SetRooms(sanitizeRooms(resolveDisplays(cmd)))
			case "set_volume":
				if cmd.VolumeDB == nil {
					log.Printf("[bridge] ignoring set_volume for %s: no volume_db", cmd.DeviceID)
					continue
				}
				if err := airplay.CheckVolumeDB(*cmd.VolumeDB); err != nil {
					log.Printf("[bridge] ignoring set_volume for %s: %v", cmd.DeviceID, err)
					continue
				}
				if a, ok := m.SetVolume(cmd.DeviceID, *cmd.VolumeDB); ok {
					// SET_PARAMETER can block on a hung receiver; keep the main loop free.
					go func(a Action) {
						if resp := d.HandleRequest(daemon.Request{Cmd: "volume", Target: a.IP, VolumeDB: a.VolumeDB}); !resp.OK {
							log.Printf("[bridge] set_volume for %s: %s", a.DeviceID, resp.Error)
						}
					}(a)
				}
			case "reconnect":
				m.Reconnect(cmd.DeviceID)
			case "restart":
				m.Restart(cmd.DeviceID)
			case "forget":
				if err := d.Forget(cmd.DeviceID); err != nil {
					log.Printf("[bridge] forget %s: %v", cmd.DeviceID, err)
				}
			case "set_lead":
				if cmd.LeadMs == nil || *cmd.LeadMs <= 0 {
					log.Printf("[bridge] ignoring set_lead for %s: no lead_ms", cmd.DeviceID)
					continue
				}
				ip, ok := m.LiveIP(cmd.DeviceID)
				if !ok {
					log.Printf("[bridge] ignoring set_lead for %s: not live", cmd.DeviceID)
					continue
				}
				// The daemon call takes its session lock; keep the main loop free.
				go func(ip string, ms int) {
					if resp := d.HandleRequest(daemon.Request{Cmd: "set_lead", Target: ip, LatencyMs: ms}); !resp.OK {
						log.Printf("[bridge] set_lead %s: %s", ip, resp.Error)
					}
				}(ip, *cmd.LeadMs)
			case "credential":
				ip := m.IP(cmd.DeviceID)
				if ip == "" || cmd.Value == "" {
					// An empty Target is a legacy daemon fallback: with a PIN it
					// delivers to some other waiting receiver, and with no PIN it
					// starts streaming to an arbitrary unselected device. Neither
					// is safe here, so refuse rather than call HandleRequest.
					log.Printf("[bridge] ignoring credential for %s: device not selected/discovered or empty value", cmd.DeviceID)
					continue
				}
				if resp := d.HandleRequest(daemon.Request{Cmd: "connect", Target: ip, Pin: cmd.Value, VolumeDB: m.VolumeDB(cmd.DeviceID), Audio: m.AudioOverride(cmd.DeviceID), WifiTolerant: m.WifiTolerant(cmd.DeviceID), LatencyMs: m.LatencyMs(cmd.DeviceID), LeadHeadroomMs: m.LeadHeadroomMs(cmd.DeviceID), AudioFormat: m.AudioFormat(cmd.DeviceID), DeviceID: cmd.DeviceID}); !resp.OK {
					log.Printf("[bridge] credential for %s rejected: %s", cmd.DeviceID, resp.Error)
				}
			default:
				log.Printf("[bridge] ignoring unknown command %q", cmd.Cmd)
			}
		case <-flush.C:
			for _, s := range dw.flush() {
				events.delivery(s)
			}
		case <-ticker.C:
			snap := snapshotFrom(d.Snapshot(), cfg.ExtraDevices)
			if !sameDevices(lastDevices, snap.Devices) {
				lastDevices = snap.Devices
				events.devices(snap.Devices)
			}
			actions, roomEvents := m.Step(cfg.Now(), snap)
			for _, a := range actions {
				switch a.Kind {
				case ActionConnect:
					if resp := d.HandleRequest(daemon.Request{Cmd: "connect", Target: a.IP, Port: a.Port, VolumeDB: a.VolumeDB, Audio: a.Audio, WifiTolerant: a.WifiTolerant, LatencyMs: a.LatencyMs, LeadHeadroomMs: a.LeadHeadroomMs, AudioFormat: a.AudioFormat, DeviceID: a.DeviceID}); !resp.OK {
						m.NoteError(a.DeviceID, resp.Error)
					}
				case ActionDisconnect:
					// Teardown can block up to the RTSP read deadline on a hung
					// receiver; never let it stall the main loop.
					go d.HandleRequest(daemon.Request{Cmd: "disconnect", Target: a.IP})
				}
			}
			for _, e := range roomEvents {
				events.room(e)
			}
		}
	}
}

// readLoop handles hello and media inline (Push never blocks) and forwards
// commands to the main loop, which owns the Manager.
func readLoop(ctx context.Context, cfg Config, in io.Reader, video *airplay.ExternalVideoSource,
	audio *airplay.ExternalAudioSource, cmds chan<- Command, readErr chan<- error) {
	helloSeen := false
	clock := newClockMap(clockMapWindow, clockMapSlewPPM, clockMapStep)
	for {
		msg, err := ReadMessage(in)
		if err != nil {
			readErr <- err
			return
		}
		if msg.Type != MsgHello && !helloSeen {
			readErr <- fmt.Errorf("bridge: message type 0x%02x before hello", msg.Type)
			return
		}
		switch msg.Type {
		case MsgHello:
			var h Hello
			if err := json.Unmarshal(msg.Payload, &h); err != nil {
				readErr <- fmt.Errorf("bridge: bad hello: %w", err)
				return
			}
			if err := h.Validate(); err != nil {
				readErr <- err
				return
			}
			helloSeen = true
		case MsgVideoAU, MsgAudioPCM:
			media, err := DecodeMedia(msg.Payload)
			if err != nil {
				readErr <- err
				return
			}
			pts := mediaTime(clock, cfg.Now(), media)
			if msg.Type == MsgVideoAU {
				_ = video.Push(airplay.VideoAccessUnit{AnnexB: media.Data, PTS: pts})
			} else if err := audio.PushCaptured(media.CaptureNs, pts, media.Data); err != nil && !errors.Is(err, airplay.ErrExternalSourceClosed) {
				readErr <- err
				return
			}
		case MsgCommand:
			var c Command
			if err := json.Unmarshal(msg.Payload, &c); err != nil {
				log.Printf("[bridge] ignoring malformed command: %v", err)
				continue
			}
			select {
			case cmds <- c:
			case <-ctx.Done():
				return
			}
		default:
			log.Printf("[bridge] ignoring unknown message type 0x%02x", msg.Type)
		}
	}
}

// snapshotFrom normalizes every discovered and extra device ID to uppercase
// before it reaches the Manager and the devices event, so a receiver mDNS
// reports in lowercase (e.g. the Android UxPlay port) still matches a room
// selected or commanded by its uppercase ID.
func snapshotFrom(s daemon.Snapshot, extra []Device) Snapshot {
	byID := make(map[string]Device)
	for _, d := range s.Devices {
		id := strings.ToUpper(d.DeviceID)
		byID[id] = Device{DeviceID: id, Name: d.Name, Model: d.Model, IP: d.IP, Port: d.Port}
	}
	for _, d := range extra {
		d.DeviceID = strings.ToUpper(d.DeviceID)
		byID[d.DeviceID] = d
	}
	out := Snapshot{LastError: s.LastError, LastErrorTarget: s.LastErrorTarget}
	for _, d := range byID {
		out.Devices = append(out.Devices, d)
	}
	sort.Slice(out.Devices, func(i, j int) bool { return out.Devices[i].DeviceID < out.Devices[j].DeviceID })
	for _, st := range s.Streams {
		out.Streams = append(out.Streams, StreamStatus{
			IP: st.DeviceIP, State: string(st.State), CredentialKind: string(st.CredentialKind),
			Audio: st.Audio, AudioReason: st.AudioReason,
		})
	}
	return out
}

func sameDevices(a, b []Device) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// ParsePortRange parses "lo-hi". Empty means OS-chosen ephemeral ports.
// doubletake needs at least three local UDP ports.
func ParsePortRange(s string) (int, int, error) {
	if s == "" {
		return 0, 0, nil
	}
	lo, hi, ok := strings.Cut(s, "-")
	if !ok {
		return 0, 0, fmt.Errorf("port range %q: want lo-hi", s)
	}
	a, err1 := strconv.Atoi(lo)
	b, err2 := strconv.Atoi(hi)
	if err1 != nil || err2 != nil || a < 1 || b > 65535 || b-a < 2 {
		return 0, 0, fmt.Errorf("port range %q: want lo-hi with at least 3 ports", s)
	}
	return a, b, nil
}

// ParseDevice parses a manual receiver entry "DEVICEID@IP:PORT" (port optional,
// default 7000) for receivers mDNS cannot reliably see.
func ParseDevice(s string) (Device, error) {
	id, addr, ok := strings.Cut(s, "@")
	if !ok || id == "" || addr == "" {
		return Device{}, fmt.Errorf("device %q: want DEVICEID@IP[:PORT]", s)
	}
	host, portText, err := net.SplitHostPort(addr)
	if err != nil {
		host, portText = addr, "7000"
	}
	if net.ParseIP(host) == nil {
		return Device{}, fmt.Errorf("device %q: %q is not an IP address", s, host)
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port < 1 || port > 65535 {
		return Device{}, fmt.Errorf("device %q: bad port %q", s, portText)
	}
	return Device{DeviceID: strings.ToUpper(id), Name: id, IP: host, Port: port}, nil
}

// ParseVolume parses -volume: "keep" leaves the receiver volume untouched;
// otherwise a level in dB from -30 to 0 (0 = full scale).
func ParseVolume(s string) (bool, float64, error) {
	if s == "" || s == "keep" {
		return false, 0, nil
	}
	db, err := strconv.ParseFloat(s, 64)
	if err != nil || db < -30 || db > 0 {
		return false, 0, fmt.Errorf("-volume %q: want keep or a dB level from -30 to 0", s)
	}
	return true, db, nil
}
