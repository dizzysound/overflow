package bridge

import (
	"testing"
	"time"
)

func TestManualAddressConnectsWithoutDiscovery(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, IP: "10.0.0.9", Port: 7001}})
	acts, evs := m.Step(t0, Snapshot{})
	if len(acts) != 1 || acts[0].Kind != ActionConnect || acts[0].IP != "10.0.0.9" || acts[0].Port != 7001 {
		t.Fatalf("acts = %+v", acts)
	}
	if lastState(t, evs, "A") != RoomConnecting {
		t.Fatalf("evs = %+v", evs)
	}
}

func TestManualAddressDefaultsToPort7000(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, IP: "10.0.0.9"}})
	acts, _ := m.Step(t0, Snapshot{})
	if len(acts) != 1 || acts[0].Port != 7000 {
		t.Fatalf("acts = %+v", acts)
	}
}

func TestManualAddressOverridesDiscovery(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, IP: "10.0.0.9"}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if got := connects(acts); len(got) != 1 || got[0] != "10.0.0.9" {
		t.Fatalf("connects = %v, want the manual address", got)
	}
}

func TestManualRoomRetriesInsteadOfGoingOffline(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, IP: "10.0.0.9"}})
	m.Step(t0, Snapshot{})
	m.Step(t0, Snapshot{Streams: []StreamStatus{stream("10.0.0.9", "streaming")}})
	_, evs := m.Step(t0, Snapshot{}) // drop
	if lastState(t, evs, "A") != RoomRetrying {
		t.Fatalf("after drop: %+v", evs)
	}
	acts, _ := m.Step(t0.Add(2*time.Second), Snapshot{})
	if got := connects(acts); len(got) != 1 || got[0] != "10.0.0.9" {
		t.Fatalf("retry connects = %v", got)
	}
}
