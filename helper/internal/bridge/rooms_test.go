package bridge

import (
	"testing"
	"time"
)

var t0 = time.Unix(1_000_000, 0)

func dev(id, ip string) Device { return Device{DeviceID: id, Name: id, IP: ip, Port: 7000} }

func stream(ip, state string) StreamStatus { return StreamStatus{IP: ip, State: state} }

func lastState(t *testing.T, evs []RoomEvent, id string) RoomState {
	t.Helper()
	var st RoomState
	for _, e := range evs {
		if e.DeviceID == id {
			st = e.State
		}
	}
	return st
}

func connects(acts []Action) []string {
	var out []string
	for _, a := range acts {
		if a.Kind == ActionConnect {
			out = append(out, a.IP)
		}
	}
	return out
}

// liveRoom brings room "A" at 10.0.0.5 to live and returns the manager.
func liveRoom(t *testing.T, auto bool) *Manager {
	t.Helper()
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: auto}})
	m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	_, evs := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}, Streams: []StreamStatus{stream("10.0.0.5", "streaming")}})
	if lastState(t, evs, "A") != RoomLive {
		t.Fatalf("setup: room not live: %+v", evs)
	}
	return m
}

func TestRoomWaitsOfflineThenConnectsWhenDiscovered(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	acts, evs := m.Step(t0, Snapshot{})
	if len(acts) != 0 || lastState(t, evs, "A") != RoomOffline {
		t.Fatalf("undiscovered: acts=%+v evs=%+v", acts, evs)
	}
	acts, evs = m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if got := connects(acts); len(got) != 1 || got[0] != "10.0.0.5" {
		t.Fatalf("connects = %v", got)
	}
	if acts[0].Port != 7000 || acts[0].DeviceID != "A" {
		t.Fatalf("action = %+v", acts[0])
	}
	if lastState(t, evs, "A") != RoomConnecting {
		t.Fatalf("state = %v", evs)
	}
}

func TestCredentialPromptCarriesKind(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	snap := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}}
	m.Step(t0, snap)
	snap.Streams = []StreamStatus{{IP: "10.0.0.5", State: "pin_required", CredentialKind: "pin"}}
	_, evs := m.Step(t0, snap)
	if len(evs) != 1 || evs[0].State != RoomCredential || evs[0].CredentialKind != "pin" {
		t.Fatalf("evs = %+v", evs)
	}
}

func TestDropWithAutoReconnectFollowsBackoff(t *testing.T) {
	m := liveRoom(t, true)
	devs := []Device{dev("A", "10.0.0.5")}
	now := t0
	for i, wait := range []time.Duration{time.Second, 2 * time.Second, 5 * time.Second, 10 * time.Second, 10 * time.Second} {
		_, evs := m.Step(now, Snapshot{Devices: devs, LastError: "boom", LastErrorTarget: "10.0.0.5"})
		if len(evs) != 1 || evs[0].State != RoomRetrying || evs[0].Error != "boom" {
			t.Fatalf("drop %d: evs = %+v", i, evs)
		}
		if acts, _ := m.Step(now.Add(wait-time.Millisecond), Snapshot{Devices: devs}); len(acts) != 0 {
			t.Fatalf("drop %d: reconnected early: %+v", i, acts)
		}
		now = now.Add(wait)
		acts, _ := m.Step(now, Snapshot{Devices: devs})
		if got := connects(acts); len(got) != 1 {
			t.Fatalf("drop %d: no reconnect at +%v", i, wait)
		}
	}
}

func TestLiveResetsBackoff(t *testing.T) {
	m := liveRoom(t, true)
	devs := []Device{dev("A", "10.0.0.5")}
	m.Step(t0, Snapshot{Devices: devs})                  // drop 1: retry at +1s
	m.Step(t0.Add(time.Second), Snapshot{Devices: devs}) // reconnect
	m.Step(t0.Add(time.Second), Snapshot{Devices: devs, Streams: []StreamStatus{stream("10.0.0.5", "streaming")}})
	m.Step(t0.Add(2*time.Second), Snapshot{Devices: devs}) // drop again
	acts, _ := m.Step(t0.Add(3*time.Second), Snapshot{Devices: devs})
	if len(connects(acts)) != 1 {
		t.Fatal("backoff did not reset to 1 s after reaching live")
	}
}

func TestDropWithoutAutoReconnectFailsUntilReconnect(t *testing.T) {
	m := liveRoom(t, false)
	devs := []Device{dev("A", "10.0.0.5")}
	_, evs := m.Step(t0, Snapshot{Devices: devs})
	if lastState(t, evs, "A") != RoomFailed {
		t.Fatalf("evs = %+v", evs)
	}
	if acts, _ := m.Step(t0.Add(time.Minute), Snapshot{Devices: devs}); len(acts) != 0 {
		t.Fatalf("auto-reconnected with toggle off: %+v", acts)
	}
	m.Reconnect("A")
	acts, _ := m.Step(t0.Add(time.Minute), Snapshot{Devices: devs})
	if len(connects(acts)) != 1 {
		t.Fatal("Reconnect did not connect")
	}
}

func TestImmediateConnectFailureUsesNotedError(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: false}})
	devs := []Device{dev("A", "10.0.0.5")}
	m.Step(t0, Snapshot{Devices: devs})
	m.NoteError("A", "connection refused")
	_, evs := m.Step(t0, Snapshot{Devices: devs})
	if len(evs) != 1 || evs[0].State != RoomFailed || evs[0].Error != "connection refused" {
		t.Fatalf("evs = %+v", evs)
	}
}

func TestDeselectDisconnectsAndGoesIdle(t *testing.T) {
	m := liveRoom(t, true)
	m.SetRooms(nil)
	acts, evs := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}, Streams: []StreamStatus{stream("10.0.0.5", "streaming")}})
	if len(acts) != 1 || acts[0].Kind != ActionDisconnect || acts[0].IP != "10.0.0.5" {
		t.Fatalf("acts = %+v", acts)
	}
	if lastState(t, evs, "A") != RoomIdle {
		t.Fatalf("evs = %+v", evs)
	}
}

func TestRetryFollowsDeviceToNewIP(t *testing.T) {
	m := liveRoom(t, true)
	m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	acts, _ := m.Step(t0.Add(time.Second), Snapshot{Devices: []Device{dev("A", "10.0.0.9")}})
	if got := connects(acts); len(got) != 1 || got[0] != "10.0.0.9" {
		t.Fatalf("connects = %v", got)
	}
}

func TestNoDuplicateEvents(t *testing.T) {
	m := liveRoom(t, true)
	snap := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}, Streams: []StreamStatus{stream("10.0.0.5", "streaming")}}
	if _, evs := m.Step(t0, snap); len(evs) != 0 {
		t.Fatalf("repeated state emitted events: %+v", evs)
	}
}

func TestStopAllDisconnectsActiveRooms(t *testing.T) {
	m := liveRoom(t, true)
	acts := m.StopAll()
	if len(acts) != 1 || acts[0].Kind != ActionDisconnect || acts[0].IP != "10.0.0.5" {
		t.Fatalf("acts = %+v", acts)
	}
}

func TestRetryingRoomGoesOfflineWhenUndiscovered(t *testing.T) {
	m := liveRoom(t, true)
	// Drop the device (no stream), room goes to retrying
	_, evs := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if lastState(t, evs, "A") != RoomRetrying {
		t.Fatalf("drop: expected RoomRetrying, got %v", lastState(t, evs, "A"))
	}
	// Step at +1s with empty devices: retrying room should go offline
	acts, evs := m.Step(t0.Add(time.Second), Snapshot{})
	if len(acts) != 0 {
		t.Fatalf("expected zero connect actions, got %+v", acts)
	}
	if lastState(t, evs, "A") != RoomOffline {
		t.Fatalf("expected RoomOffline, got %v", lastState(t, evs, "A"))
	}
	// Rediscover device at new IP: should connect
	acts, _ = m.Step(t0.Add(time.Second), Snapshot{Devices: []Device{dev("A", "10.0.0.7")}})
	if got := connects(acts); len(got) != 1 || got[0] != "10.0.0.7" {
		t.Fatalf("expected connect to 10.0.0.7, got %v", got)
	}
}

func TestEnablingAutoReconnectRearmsFailedRoom(t *testing.T) {
	m := liveRoom(t, false)
	devs := []Device{dev("A", "10.0.0.5")}
	// Drop the device: with autoReconnect=false, goes to failed
	_, evs := m.Step(t0, Snapshot{Devices: devs})
	if lastState(t, evs, "A") != RoomFailed {
		t.Fatalf("drop: expected RoomFailed, got %v", lastState(t, evs, "A"))
	}
	// Enable autoReconnect: room should go offline and be ready to connect
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	acts, _ := m.Step(t0, Snapshot{Devices: devs})
	if got := connects(acts); len(got) != 1 {
		t.Fatalf("expected one connect action after enabling auto-reconnect, got %v", got)
	}
}

func TestLiveClearsStaleNotedError(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	m.NoteError("A", "stale synchronous error")
	_, evs := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}, Streams: []StreamStatus{stream("10.0.0.5", "streaming")}})
	if lastState(t, evs, "A") != RoomLive {
		t.Fatalf("room not live: %+v", evs)
	}
	_, evs = m.Step(t0.Add(time.Second), Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(evs) != 1 || evs[0].State != RoomRetrying || evs[0].Error != "disconnected" {
		t.Fatalf("drop after live = %+v, want retrying with error \"disconnected\"", evs)
	}
}

const rejectionSuffix = " (auto-reconnect paused: restart the receiver, then reconnect)"

func TestReceiverRejectionFailsDespiteAutoReconnect(t *testing.T) {
	m := liveRoom(t, true)
	devs := []Device{dev("A", "10.0.0.5")}
	wantErr := "FairPlay setup failed: fp-setup: HTTP 403" + rejectionSuffix
	_, evs := m.Step(t0, Snapshot{Devices: devs, LastError: "FairPlay setup failed: fp-setup: HTTP 403", LastErrorTarget: "10.0.0.5"})
	if len(evs) != 1 || evs[0].State != RoomFailed || evs[0].Error != wantErr {
		t.Fatalf("evs = %+v, want failed with %q", evs, wantErr)
	}
	// No connect action even long after the normal backoff would have fired.
	if acts, _ := m.Step(t0.Add(time.Minute), Snapshot{Devices: devs}); len(acts) != 0 {
		t.Fatalf("auto-reconnected after rejection: %+v", acts)
	}
	m.Reconnect("A")
	acts, _ := m.Step(t0.Add(time.Minute), Snapshot{Devices: devs})
	if len(connects(acts)) != 1 {
		t.Fatal("Reconnect did not connect after rejection")
	}
}

func TestOrdinaryDropWithAutoReconnectStillRetries(t *testing.T) {
	m := liveRoom(t, true)
	devs := []Device{dev("A", "10.0.0.5")}
	_, evs := m.Step(t0, Snapshot{Devices: devs, LastError: "connect to 10.0.0.5:7000 failed: connection refused", LastErrorTarget: "10.0.0.5"})
	if len(evs) != 1 || evs[0].State != RoomRetrying || evs[0].Error != "connect to 10.0.0.5:7000 failed: connection refused" {
		t.Fatalf("evs = %+v, want retrying unchanged", evs)
	}
}

func TestNoteErrorRejectionFailsSynchronously(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	devs := []Device{dev("A", "10.0.0.5")}
	m.Step(t0, Snapshot{Devices: devs})
	m.NoteError("A", "transient pairing failed for password-protected receiver: pair-setup: HTTP 470")
	_, evs := m.Step(t0, Snapshot{Devices: devs})
	wantErr := "transient pairing failed for password-protected receiver: pair-setup: HTTP 470" + rejectionSuffix
	if len(evs) != 1 || evs[0].State != RoomFailed || evs[0].Error != wantErr {
		t.Fatalf("evs = %+v, want failed with %q", evs, wantErr)
	}
}

func TestFairPlayTimeoutDropFailsDespiteAutoReconnect(t *testing.T) {
	m := liveRoom(t, true)
	devs := []Device{dev("A", "10.0.0.5")}
	msg := "FairPlay setup failed: fp-setup phase 1 (m1): read tcp 10.0.0.5:7000: i/o timeout"
	wantErr := msg + rejectionSuffix
	_, evs := m.Step(t0, Snapshot{Devices: devs, LastError: msg, LastErrorTarget: "10.0.0.5"})
	if len(evs) != 1 || evs[0].State != RoomFailed || evs[0].Error != wantErr {
		t.Fatalf("evs = %+v, want failed with %q (an intermittent TCP timeout during fp-setup must not be retried)", evs, wantErr)
	}
}

func TestIsReceiverRejection(t *testing.T) {
	cases := []struct {
		name string
		msg  string
		want bool
	}{
		{"fairplay setup failed", "FairPlay setup failed: fp-setup phase 1 (m1): HTTP 403 (body: )", true},
		{"fairplay setup failed no status", "FairPlay setup failed: some other transport error", true},
		{"pairing http 403", "transient pairing failed for password-protected receiver: pair-setup: HTTP 403 (body: )", true},
		{"pairing http 470", "reconnect for credential pairing failed: pair-verify: HTTP 470 (body: )", true},
		{"mirror setup http 403", "mirror setup failed: HTTP 403 (body: )", true},
		{"connection refused", "connect to 10.0.0.5:7000 failed: connection refused", false},
		{"io timeout", "mirror setup failed: i/o timeout", false},
		{"eof", "connect to 10.0.0.5:7000 failed: EOF", false},
		{"broken pipe", "stream error after live: write: broken pipe", false},
		{"disconnected", "disconnected", false},
		{"wrong pin", "wait for password: some prompt", false},
		{"pair-setup TLV error not http", "PIN pairing: pair-setup M2 error: authentication failed (2)", false},
		{"fairplay setup failed with io timeout", "FairPlay setup failed: fp-setup phase 1 (m1): read tcp 10.0.0.5:7000: i/o timeout", true},
		{"fairplay setup failed with EOF", "FairPlay setup failed: fp-setup phase 2 (m3): read tcp 10.0.0.5:7000: EOF", true},
		{"http 403 with broken pipe", "mirror setup failed: HTTP 403 (body: writev: broken pipe)", true},
		{"plain connection refused", "connect to 10.0.0.5:7000 failed: connection refused", false},
		{"pair-setup M4 wrong pin", "PIN pairing: pair-setup: pair-setup M4 error: 2", false},
		{"empty", "", false},
	}
	for _, c := range cases {
		if got := isReceiverRejection(c.msg); got != c.want {
			t.Errorf("%s: isReceiverRejection(%q) = %v, want %v", c.name, c.msg, got, c.want)
		}
	}
}
