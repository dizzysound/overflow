package bridge

import "testing"

func ms(v int) *int { return &v }

func TestRoomLatencyRidesOnConnect(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, LatencyMs: ms(95)}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].Kind != ActionConnect || acts[0].LatencyMs != 95 {
		t.Fatalf("acts = %+v, want one connect with LatencyMs 95", acts)
	}
	if m.LatencyMs("A") != 95 || m.LatencyMs("nope") != 0 {
		t.Fatalf("LatencyMs accessor wrong")
	}
}

func TestRoomWithoutLatencyConnectsWithZero(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].LatencyMs != 0 {
		t.Fatalf("acts = %+v, want LatencyMs 0 (helper default)", acts)
	}
}
