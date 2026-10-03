package bridge

import (
	"testing"
	"time"
)

func off() *bool { f := false; return &f }

func TestRoomAudioOffRidesOnConnect(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, Audio: off()}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].Kind != ActionConnect || acts[0].Audio == nil || *acts[0].Audio {
		t.Fatalf("acts = %+v", acts)
	}
}

func TestRoomWithoutAudioFieldConnectsWithoutOverride(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].Audio != nil {
		t.Fatalf("acts = %+v, want no audio override", acts)
	}
}

func TestRoomAudioTrueConnectsWithoutOverride(t *testing.T) {
	on := true
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, Audio: &on}})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5")}})
	if len(acts) != 1 || acts[0].Audio != nil {
		t.Fatalf("acts = %+v, want no audio override (audio:true never forces audio on)", acts)
	}
}

func TestRoomAudioOffSurvivesReconnect(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, Audio: off()}})
	discovered := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}}
	acts, _ := m.Step(t0, discovered)
	if len(acts) != 1 || acts[0].Audio == nil || *acts[0].Audio {
		t.Fatalf("initial connect acts = %+v", acts)
	}
	// The stream never came up (no Streams in the snapshot); it drops and retries.
	m.Step(t0, discovered)
	acts, _ = m.Step(t0.Add(2*time.Second), discovered)
	if len(acts) != 1 || acts[0].Kind != ActionConnect || acts[0].Audio == nil || *acts[0].Audio {
		t.Fatalf("reconnect acts = %+v, want the audio override to survive", acts)
	}
}

func TestRoomAudioOverrideForCredentialConnect(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, Audio: off()}})
	if a := m.AudioOverride("A"); a == nil || *a {
		t.Fatalf("AudioOverride(A) = %v, want false pointer", a)
	}
	if a := m.AudioOverride("nope"); a != nil {
		t.Fatalf("AudioOverride(unknown) = %v, want nil", *a)
	}
}

func TestSetRoomsReplacesStoredAudioOverride(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true, Audio: off()}})
	if a := m.AudioOverride("A"); a == nil || *a {
		t.Fatalf("AudioOverride(A) = %v, want false pointer", a)
	}
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	if a := m.AudioOverride("A"); a != nil {
		t.Fatalf("AudioOverride(A) = %v, want nil after set_rooms without audio:false", *a)
	}
}
