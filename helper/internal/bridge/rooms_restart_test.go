package bridge

import "testing"

func TestRestartLiveRoomDisconnectsThenReconnects(t *testing.T) {
	m := liveRoom(t, true)
	m.Restart("A")
	live := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}, Streams: []StreamStatus{stream("10.0.0.5", "streaming")}}

	acts, evs := m.Step(t0, live)
	if len(acts) != 1 || acts[0].Kind != ActionDisconnect || acts[0].IP != "10.0.0.5" {
		t.Fatalf("first step acts = %+v, want one disconnect", acts)
	}
	if lastState(t, evs, "A") != RoomConnecting {
		t.Fatalf("first step evs = %+v, want connecting", evs)
	}

	// The old session is still listed: hold, and do not connect a duplicate.
	acts, evs = m.Step(t0, live)
	if len(acts) != 0 || len(evs) != 0 {
		t.Fatalf("while the old session tears down: acts=%+v evs=%+v", acts, evs)
	}

	// The old session is gone: connect at once.
	acts, _ = m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if got := connects(acts); len(got) != 1 || got[0] != "10.0.0.5" {
		t.Fatalf("after teardown acts = %+v, want one connect", acts)
	}
}

func TestRestartRetryingRoomSkipsBackoff(t *testing.T) {
	m := liveRoom(t, true)
	discovered := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}}
	m.Step(t0, discovered) // drop: retrying, next attempt at t0+1s
	m.Restart("A")
	acts, _ := m.Step(t0, discovered)
	if got := connects(acts); len(got) != 1 {
		t.Fatalf("restart did not connect immediately: %+v", acts)
	}
}

func TestRestartFailedRoomReconnects(t *testing.T) {
	m := liveRoom(t, false)
	discovered := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}}
	_, evs := m.Step(t0, discovered)
	if lastState(t, evs, "A") != RoomFailed {
		t.Fatalf("setup: evs = %+v, want failed", evs)
	}
	m.Restart("A")
	acts, evs := m.Step(t0, discovered)
	if got := connects(acts); len(got) != 1 || lastState(t, evs, "A") != RoomConnecting {
		t.Fatalf("acts=%+v evs=%+v", acts, evs)
	}
}

func TestRestartUnknownRoomIsIgnored(t *testing.T) {
	m := NewManager()
	m.Restart("X")
	acts, evs := m.Step(t0, Snapshot{})
	if len(acts) != 0 || len(evs) != 0 {
		t.Fatalf("acts=%+v evs=%+v", acts, evs)
	}
}

// The plugin's ceiling guard (2026-10-04: audio age above a live display's
// announced ceiling): set_displays with a higher latency_ms leaves the live
// session alone, and restart reconnects it with the new lead and headroom.
func TestRestartAfterRaisedLatencyReconnectsWithTheNewLead(t *testing.T) {
	m := NewManager()
	headroom := 100
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, LatencyMs: ms(197), LeadHeadroomMs: &headroom}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].Kind != ActionConnect || acts[0].LatencyMs != 197 || acts[0].LeadHeadroomMs != 100 {
		t.Fatalf("first connect %+v, want lead 197 headroom 100", acts)
	}
	live := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}, Streams: []StreamStatus{stream("10.0.0.5", "streaming")}}
	if _, evs := m.Step(t0, live); lastState(t, evs, "A") != RoomLive {
		t.Fatalf("not live: %+v", evs)
	}

	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, LatencyMs: ms(520), LeadHeadroomMs: &headroom}})
	if acts, _ := m.Step(t0, live); len(acts) != 0 {
		t.Fatalf("a new latency_ms alone touched the live session: %+v", acts)
	}
	m.Restart("A")
	if acts, _ := m.Step(t0, live); len(acts) != 1 || acts[0].Kind != ActionDisconnect {
		t.Fatalf("restart acts %+v, want one disconnect", acts)
	}
	acts, _ = m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].Kind != ActionConnect || acts[0].LatencyMs != 520 || acts[0].LeadHeadroomMs != 100 {
		t.Fatalf("reconnect %+v, want lead 520 headroom 100", acts)
	}
}
