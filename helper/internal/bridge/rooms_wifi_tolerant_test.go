package bridge

import (
	"testing"
	"time"
)

func yes() *bool { t := true; return &t }

func TestRoomWifiTolerantRidesOnConnect(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, WifiTolerant: yes()}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].Kind != ActionConnect || !acts[0].WifiTolerant {
		t.Fatalf("acts = %+v", acts)
	}
}

func TestRoomWithoutWifiTolerantFieldConnectsWithDefaultBudget(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{
		{DeviceID: "A", AutoReconnect: true},
		{DeviceID: "B", AutoReconnect: true, WifiTolerant: off()},
	})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5"), dev("B", "10.0.0.6")}})
	if len(acts) != 2 || acts[0].WifiTolerant || acts[1].WifiTolerant {
		t.Fatalf("acts = %+v, want neither connect Wi-Fi tolerant", acts)
	}
}

func TestRoomWifiTolerantSurvivesReconnect(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, WifiTolerant: yes()}})
	discovered := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}}
	acts, _ := m.Step(t0, discovered)
	if len(acts) != 1 || !acts[0].WifiTolerant {
		t.Fatalf("initial connect acts = %+v", acts)
	}
	// The stream never came up (no Streams in the snapshot); it drops and retries.
	m.Step(t0, discovered)
	acts, _ = m.Step(t0.Add(2*time.Second), discovered)
	if len(acts) != 1 || acts[0].Kind != ActionConnect || !acts[0].WifiTolerant {
		t.Fatalf("reconnect acts = %+v, want wifi_tolerant to survive", acts)
	}
}

func TestRoomWifiTolerantForCredentialConnect(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, WifiTolerant: yes()}})
	if !m.WifiTolerant("A") {
		t.Fatal("WifiTolerant(A) = false, want true")
	}
	if m.WifiTolerant("nope") {
		t.Fatal("WifiTolerant(unknown) = true, want false")
	}
}

func TestSetRoomsReplacesStoredWifiTolerant(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, WifiTolerant: yes()}})
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	if m.WifiTolerant("A") {
		t.Fatal("WifiTolerant(A) = true after set_rooms without wifi_tolerant, want false")
	}
}
