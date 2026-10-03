package bridge

import "testing"

func TestRoomAudioFormatRidesOnConnect(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{
		{DeviceID: "A", AutoReconnect: true, AudioFormat: "aac-eld"},
		{DeviceID: "B", AutoReconnect: true, AudioFormat: "auto"},
	})
	acts, _ := m.Step(t0, Snapshot{Devices: []Device{dev("A", "10.0.0.5"), dev("B", "10.0.0.6")}})
	if len(acts) != 2 || acts[0].AudioFormat != "aac-eld" || acts[1].AudioFormat != "" {
		t.Fatalf("acts = %+v, want A aac-eld and B automatic", acts)
	}
	if m.AudioFormat("A") != "aac-eld" || m.AudioFormat("nope") != "" {
		t.Fatalf("AudioFormat(A) = %q, AudioFormat(unknown) = %q", m.AudioFormat("A"), m.AudioFormat("nope"))
	}
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	if m.AudioFormat("A") != "" {
		t.Fatalf("AudioFormat(A) after reselect without the field = %q, want automatic", m.AudioFormat("A"))
	}
}
