package bridge

import (
	"sort"
	"strings"
	"time"
)

type RoomState string

const (
	RoomIdle       RoomState = "idle"
	RoomOffline    RoomState = "offline"
	RoomConnecting RoomState = "connecting"
	RoomCredential RoomState = "credential"
	RoomLive       RoomState = "live"
	RoomRetrying   RoomState = "retrying"
	RoomFailed     RoomState = "failed"
)

type Device struct {
	DeviceID string `json:"device_id"`
	Name     string `json:"name"`
	Model    string `json:"model"`
	IP       string `json:"ip"`
	Port     int    `json:"port"`
}

// StreamStatus mirrors one daemon stream; State uses daemon values
// ("connecting", "pin_required", "streaming"). Audio is "on", "off" or "".
type StreamStatus struct {
	IP             string
	State          string
	CredentialKind string
	Audio          string
	AudioReason    string
}

type Snapshot struct {
	Devices         []Device
	Streams         []StreamStatus
	LastError       string
	LastErrorTarget string
}

type ActionKind int

const (
	ActionConnect ActionKind = iota
	ActionDisconnect
	ActionSetVolume
)

type Action struct {
	Kind     ActionKind
	DeviceID string
	IP       string
	Port     int
	VolumeDB *float64 // ActionConnect: level for session start (nil = none); ActionSetVolume: level to apply
	// Audio is set only on ActionConnect: nil leaves the daemon's default audio
	// behavior alone; a non-nil false turns audio off for this display's
	// session. It is never non-nil true -- audio:true never overrides a global
	// no-audio setting, it only means "do not turn this display's audio off".
	Audio *bool
	// WifiTolerant is set only on ActionConnect: true gives this display's
	// session the longer Wi-Fi tolerant relay budget.
	WifiTolerant bool
	// LatencyMs is set only on ActionConnect: this display's playout lead, 0 = helper default.
	LatencyMs int
	// LeadHeadroomMs is set only on ActionConnect: how far this display's lead may
	// slide up while live, 0 = fixed lead.
	LeadHeadroomMs int
	// AudioFormat is set only on ActionConnect: forced screen-audio codec, "" = automatic.
	AudioFormat string
}

type RoomEvent struct {
	DeviceID       string    `json:"device_id"`
	State          RoomState `json:"state"`
	CredentialKind string    `json:"credential_kind,omitempty"`
	Error          string    `json:"error,omitempty"`
	Audio          string    `json:"audio,omitempty"`        // live only: "on" or "off"
	AudioReason    string    `json:"audio_reason,omitempty"` // live with audio "off"
}

var backoffSchedule = []time.Duration{time.Second, 2 * time.Second, 5 * time.Second, 10 * time.Second}

// receiverRejectionSuffix is appended to the reported error when a room is
// failed because of a receiver rejection rather than an ordinary drop, so the
// operator knows retrying automatically will not help.
const receiverRejectionSuffix = " (auto-reconnect paused: restart the receiver, then reconnect)"

// isReceiverRejection reports whether msg describes a FairPlay or pairing
// rejection from the receiver itself, as opposed to an ordinary network drop
// or a wrong PIN/password that the user can retype. Repeatedly retrying a
// rejection (e.g. FairPlay /fp-setup returning HTTP 403) has been observed to
// lock an Apple TV out until it is restarted, so these must not be retried
// automatically.
//
// Classification is based on the daemon's error strings (see
// internal/daemon/daemon.go removeStream call sites) and the underlying
// transport error, which stringifies as "HTTP <code> (body: ...)" via
// airplay.HTTPStatusError (internal/airplay/client.go).
func isReceiverRejection(msg string) bool {
	if msg == "" {
		return false
	}

	// Any FairPlay setup failure is a rejection, whatever else the message
	// contains: FairPlay does not offer a retry-with-new-credential path the
	// way pairing does, and repeated /fp-setup attempts (even ones that time
	// out, per the evidence) have locked an Apple TV out until restarted.
	if strings.Contains(msg, "FairPlay setup failed") {
		return true
	}

	// A pairing or mirror-setup message where the receiver itself rejected the
	// request (HTTP 403 or 470), as opposed to a TLV8 pairing error code or a
	// transport-level failure. Checked before the network/password exclusions
	// below so that, e.g., a 403 alongside "broken pipe" still counts as a
	// rejection.
	if strings.Contains(msg, "HTTP 403") || strings.Contains(msg, "HTTP 470") {
		return true
	}

	lower := strings.ToLower(msg)

	// A wrong PIN or password re-prompts the user; keep retrying.
	if strings.Contains(lower, "wait for password") {
		return false
	}

	// Network errors are ordinary drops. Matched only here, after the
	// FairPlay/HTTP-status checks above, so an FairPlay/HTTP rejection is
	// never misclassified as an ordinary drop just because the underlying
	// transport error also mentions a timeout or a closed connection (the
	// intermittent-timeout lockout symptom from the evidence).
	if strings.Contains(lower, "connection refused") || strings.Contains(lower, "i/o timeout") || strings.Contains(lower, "broken pipe") {
		return false
	}
	// EOF is matched narrowly (": EOF" or a trailing "EOF") so this does not
	// also match unrelated words containing "eof".
	if strings.HasSuffix(msg, "EOF") || strings.Contains(msg, ": EOF") {
		return false
	}

	return false
}

type room struct {
	deviceID       string
	autoReconnect  bool
	ip             string
	port           int
	state          RoomState
	credKind       string
	errMsg         string
	notedErr       string
	attempts       int
	retryAt        time.Time
	volumeDB       *float64
	audioOff       bool   // set_displays audio:false: this display streams video only
	wifiTolerant   bool   // set_displays wifi_tolerant:true: longer relay budget
	latencyMs      int    // set_displays latency_ms: playout lead, 0 = helper default
	leadHeadroomMs int    // set_displays lead_headroom_ms: how far the lead may slide up, 0 = fixed
	audioFormat    string // set_displays audio_format: forced screen-audio codec, "" = automatic
	audio          string
	audioReason    string
	restarting     bool   // Restart was requested; wait for the old stream to leave the snapshot
	manualIP       string // set_rooms ip: used instead of discovery
	manualPort     int
}

// Manager decides, from periodic daemon snapshots, which receivers to connect
// or disconnect and which room events to report. It holds no locks and does no
// I/O; the bridge calls it from a single goroutine.
type Manager struct {
	rooms       map[string]*room
	disconnects []Action
	removed     []string
	lastEmitted map[string]RoomEvent
}

func NewManager() *Manager {
	return &Manager{rooms: make(map[string]*room), lastEmitted: make(map[string]RoomEvent)}
}

func (r *room) active() bool {
	return r.state == RoomConnecting || r.state == RoomCredential || r.state == RoomLive
}

// SetRooms replaces the selection. Deselected rooms are disconnected on the
// next Step; the auto-reconnect toggle and per-room options update in place
// without resetting state. Each entry's volume_db replaces the stored level
// (absent clears it).
func (m *Manager) SetRooms(sel []RoomSelection) {
	want := make(map[string]bool, len(sel))
	for _, s := range sel {
		want[s.DeviceID] = true
		r, ok := m.rooms[s.DeviceID]
		if !ok {
			r = &room{deviceID: s.DeviceID, state: RoomOffline}
			m.rooms[s.DeviceID] = r
		} else if !r.autoReconnect && s.AutoReconnect && r.state == RoomFailed {
			// Enabling auto-reconnect re-arms a failed room.
			r.state = RoomOffline
			r.attempts = 0
		}
		r.autoReconnect = s.AutoReconnect
		r.volumeDB = copyVolume(s.VolumeDB)
		r.audioOff = s.Audio != nil && !*s.Audio
		r.wifiTolerant = s.WifiTolerant != nil && *s.WifiTolerant
		r.latencyMs = 0
		if s.LatencyMs != nil && *s.LatencyMs > 0 {
			r.latencyMs = *s.LatencyMs
		}
		r.leadHeadroomMs = 0
		if s.LeadHeadroomMs != nil && *s.LeadHeadroomMs > 0 && *s.LeadHeadroomMs <= 500 {
			r.leadHeadroomMs = *s.LeadHeadroomMs
		}
		r.audioFormat = s.AudioFormat
		if r.audioFormat == "auto" {
			r.audioFormat = ""
		}
		r.manualIP, r.manualPort = s.IP, s.Port
		if r.manualIP != "" && r.manualPort == 0 {
			r.manualPort = 7000
		}
	}
	for id, r := range m.rooms {
		if want[id] {
			continue
		}
		if r.active() && r.ip != "" {
			m.disconnects = append(m.disconnects, Action{Kind: ActionDisconnect, DeviceID: id, IP: r.ip, Port: r.port})
		}
		delete(m.rooms, id)
		m.removed = append(m.removed, id)
	}
}

// SetVolume stores a room's volume for its later sessions. When the room is
// live it also returns the action that applies the level now.
func (m *Manager) SetVolume(deviceID string, db float64) (Action, bool) {
	r, ok := m.rooms[deviceID]
	if !ok {
		return Action{}, false
	}
	r.volumeDB = copyVolume(&db)
	if r.state != RoomLive || r.ip == "" {
		return Action{}, false
	}
	return Action{Kind: ActionSetVolume, DeviceID: deviceID, IP: r.ip, Port: r.port, VolumeDB: copyVolume(&db)}, true
}

// VolumeDB returns a copy of the room's stored volume, or nil.
func (m *Manager) VolumeDB(deviceID string) *float64 {
	if r, ok := m.rooms[deviceID]; ok {
		return copyVolume(r.volumeDB)
	}
	return nil
}

func copyVolume(v *float64) *float64 {
	if v == nil {
		return nil
	}
	c := *v
	return &c
}

// audioOverride returns the connect Action's Audio override for a room whose
// audio is off, or nil when it is on -- leaving the daemon's own default
// (Config.NoAudio) alone rather than ever forcing audio on.
func audioOverride(off bool) *bool {
	if !off {
		return nil
	}
	f := false
	return &f
}

// AudioOverride returns the connect-request Audio override for deviceID (see
// audioOverride), or nil if the room is unknown or its audio is on.
func (m *Manager) AudioOverride(deviceID string) *bool {
	if r, ok := m.rooms[deviceID]; ok {
		return audioOverride(r.audioOff)
	}
	return nil
}

// WifiTolerant reports whether deviceID's sessions use the Wi-Fi tolerant
// relay budget; false if the room is unknown.
func (m *Manager) WifiTolerant(deviceID string) bool {
	if r, ok := m.rooms[deviceID]; ok {
		return r.wifiTolerant
	}
	return false
}

// LatencyMs returns deviceID's playout lead in ms (0: helper default).
func (m *Manager) LatencyMs(deviceID string) int {
	if r, ok := m.rooms[deviceID]; ok {
		return r.latencyMs
	}
	return 0
}

// AudioFormat returns deviceID's forced screen-audio codec ("" = automatic).
func (m *Manager) AudioFormat(deviceID string) string {
	if r, ok := m.rooms[deviceID]; ok {
		return r.audioFormat
	}
	return ""
}

// Reconnect retries a failed or retrying room on the next Step.
func (m *Manager) Reconnect(deviceID string) {
	if r, ok := m.rooms[deviceID]; ok && (r.state == RoomFailed || r.state == RoomRetrying) {
		r.state = RoomOffline
		r.attempts = 0
	}
}

// Restart ends a selected room's session, whatever its state, and connects
// again as soon as the old stream is gone, with the backoff reset. A TV woken
// from standby keeps audio but loses video until a new session sends the codec
// configuration, so the operator needs this.
func (m *Manager) Restart(deviceID string) {
	r, ok := m.rooms[deviceID]
	if !ok {
		return
	}
	if r.active() && r.ip != "" {
		m.disconnects = append(m.disconnects, Action{Kind: ActionDisconnect, DeviceID: deviceID, IP: r.ip, Port: r.port})
	}
	r.state = RoomOffline
	r.restarting = true
	r.attempts = 0
	r.errMsg, r.notedErr, r.credKind = "", "", ""
	r.audio, r.audioReason = "", ""
}

// emit appends r's room event to evs when it differs from the last one sent.
func (m *Manager) emit(id string, r *room, evs []RoomEvent) []RoomEvent {
	ev := RoomEvent{DeviceID: id, State: r.state, CredentialKind: r.credKind}
	if r.state == RoomRetrying || r.state == RoomFailed {
		ev.Error = r.errMsg
	}
	if r.state == RoomLive {
		ev.Audio = r.audio
		if r.audio == "off" {
			ev.AudioReason = r.audioReason
		}
	}
	if prev, ok := m.lastEmitted[id]; !ok || prev != ev {
		m.lastEmitted[id] = ev
		evs = append(evs, ev)
	}
	return evs
}

// NoteError records a synchronous connect failure for the next Step to report.
func (m *Manager) NoteError(deviceID, msg string) {
	if r, ok := m.rooms[deviceID]; ok {
		r.notedErr = msg
	}
}

func (m *Manager) IP(deviceID string) string {
	if r, ok := m.rooms[deviceID]; ok {
		return r.ip
	}
	return ""
}

// LeadHeadroomMs returns deviceID's lead headroom in ms (0: fixed lead).
func (m *Manager) LeadHeadroomMs(deviceID string) int {
	if r, ok := m.rooms[deviceID]; ok {
		return r.leadHeadroomMs
	}
	return 0
}

// LiveIP returns deviceID's address while its session is live.
func (m *Manager) LiveIP(deviceID string) (string, bool) {
	r, ok := m.rooms[deviceID]
	if !ok || r.state != RoomLive || r.ip == "" {
		return "", false
	}
	return r.ip, true
}

// StopAll returns disconnect actions for every active room.
func (m *Manager) StopAll() []Action {
	var acts []Action
	for id, r := range m.rooms {
		if r.active() && r.ip != "" {
			acts = append(acts, Action{Kind: ActionDisconnect, DeviceID: id, IP: r.ip, Port: r.port})
		}
	}
	return acts
}

func (m *Manager) Step(now time.Time, snap Snapshot) ([]Action, []RoomEvent) {
	acts := m.disconnects
	m.disconnects = nil
	var evs []RoomEvent
	for _, id := range m.removed {
		evs = append(evs, RoomEvent{DeviceID: id, State: RoomIdle})
		delete(m.lastEmitted, id)
	}
	m.removed = nil

	devices := make(map[string]Device, len(snap.Devices))
	for _, d := range snap.Devices {
		devices[d.DeviceID] = d
	}
	streams := make(map[string]StreamStatus, len(snap.Streams))
	for _, s := range snap.Streams {
		streams[s.IP] = s
	}

	ids := make([]string, 0, len(m.rooms))
	for id := range m.rooms {
		ids = append(ids, id)
	}
	sort.Strings(ids)

	for _, id := range ids {
		r := m.rooms[id]
		if r.restarting {
			if _, still := streams[r.ip]; still && r.ip != "" {
				// The old session is still tearing down; the daemon would
				// refuse a second connect to the same address.
				r.state = RoomConnecting
				evs = m.emit(id, r, evs)
				continue
			}
			r.restarting = false
			r.state = RoomOffline
		}
		d, deviceFound := devices[id]
		if r.manualIP != "" {
			// A manual address (set_rooms ip/port) stands in for discovery.
			d, deviceFound = Device{DeviceID: id, IP: r.manualIP, Port: r.manualPort}, true
		}
		if deviceFound && !r.active() {
			// Keep the old address while a stream is up; follow DHCP changes otherwise.
			r.ip, r.port = d.IP, d.Port
		}
		st, streaming := streams[r.ip]
		switch {
		case r.ip != "" && streaming:
			r.credKind = ""
			switch st.State {
			case "streaming":
				r.state, r.errMsg, r.attempts = RoomLive, "", 0
				r.audio, r.audioReason = st.Audio, st.AudioReason
				// A synchronous error from an earlier attempt must not be
				// reported on a later real drop.
				r.notedErr = ""
			case "pin_required":
				r.state, r.credKind = RoomCredential, st.CredentialKind
			default:
				r.state = RoomConnecting
			}
		case r.active():
			// The daemon removed the stream: it failed or the receiver ended it.
			r.credKind = ""
			r.errMsg = "disconnected"
			// Synchronous error (notedErr) is more specific than daemon's last asynchronous error.
			if r.notedErr != "" {
				r.errMsg = r.notedErr
			} else if snap.LastError != "" && snap.LastErrorTarget == r.ip {
				r.errMsg = snap.LastError
			}
			r.notedErr = ""
			if r.autoReconnect && !isReceiverRejection(r.errMsg) {
				wait := backoffSchedule[min(r.attempts, len(backoffSchedule)-1)]
				r.attempts++
				r.state, r.retryAt = RoomRetrying, now.Add(wait)
			} else {
				r.state = RoomFailed
				if r.autoReconnect {
					r.errMsg += receiverRejectionSuffix
				}
			}
		case r.state == RoomRetrying && !deviceFound:
			// Undiscovered retrying room goes offline.
			r.state = RoomOffline
			r.ip, r.port = "", 0
		case r.state == RoomRetrying && !now.Before(r.retryAt) && r.ip != "":
			acts = append(acts, Action{Kind: ActionConnect, DeviceID: id, IP: r.ip, Port: r.port, VolumeDB: copyVolume(r.volumeDB), Audio: audioOverride(r.audioOff), WifiTolerant: r.wifiTolerant, LatencyMs: r.latencyMs, LeadHeadroomMs: r.leadHeadroomMs, AudioFormat: r.audioFormat})
			r.state = RoomConnecting
		case r.state == RoomOffline && r.ip != "":
			acts = append(acts, Action{Kind: ActionConnect, DeviceID: id, IP: r.ip, Port: r.port, VolumeDB: copyVolume(r.volumeDB), Audio: audioOverride(r.audioOff), WifiTolerant: r.wifiTolerant, LatencyMs: r.latencyMs, LeadHeadroomMs: r.leadHeadroomMs, AudioFormat: r.audioFormat})
			r.state = RoomConnecting
		}

		evs = m.emit(id, r, evs)
	}
	return acts, evs
}
