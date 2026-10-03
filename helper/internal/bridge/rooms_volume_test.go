package bridge

import (
	"testing"
	"time"
)

func vol(db float64) *float64 { return &db }

func TestRoomVolumeRidesOnConnect(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, VolumeDB: vol(-12)}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].Kind != ActionConnect || acts[0].VolumeDB == nil || *acts[0].VolumeDB != -12 {
		t.Fatalf("acts = %+v", acts)
	}
}

func TestRoomWithoutVolumeConnectsWithoutOne(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].VolumeDB != nil {
		t.Fatalf("acts = %+v", acts)
	}
}

func TestSetVolumeOnLiveRoomAppliesNowAndOnReconnect(t *testing.T) {
	m := liveRoom(t, true)
	a, ok := m.SetVolume("A", -20)
	if !ok || a.Kind != ActionSetVolume || a.IP != "10.0.0.5" || a.VolumeDB == nil || *a.VolumeDB != -20 {
		t.Fatalf("SetVolume = %+v, %v", a, ok)
	}
	discovered := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}}
	m.Step(t0, discovered) // the stream drops; retry in 1 s
	acts, _ := m.Step(t0.Add(2*time.Second), discovered)
	if len(acts) != 1 || acts[0].Kind != ActionConnect || acts[0].VolumeDB == nil || *acts[0].VolumeDB != -20 {
		t.Fatalf("reconnect acts = %+v", acts)
	}
}

func TestSetVolumeOnOfflineRoomOnlyStores(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	if a, ok := m.SetVolume("A", -6); ok {
		t.Fatalf("offline room returned a live action %+v", a)
	}
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].VolumeDB == nil || *acts[0].VolumeDB != -6 {
		t.Fatalf("acts = %+v", acts)
	}
}

func TestSetVolumeUnknownRoomIsIgnored(t *testing.T) {
	m := NewManager()
	if _, ok := m.SetVolume("nope", -6); ok {
		t.Fatal("unknown room returned an action")
	}
	if v := m.VolumeDB("nope"); v != nil {
		t.Fatalf("VolumeDB(unknown) = %v, want nil", *v)
	}
}

func TestSetRoomsReplacesStoredVolume(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, VolumeDB: vol(-12)}})
	m.SetVolume("A", -3)
	if v := m.VolumeDB("A"); v == nil || *v != -3 {
		t.Fatalf("after set_volume: %v", v)
	}
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	if v := m.VolumeDB("A"); v != nil {
		t.Fatalf("volume = %v, want nil after set_rooms without volume_db", *v)
	}
}
