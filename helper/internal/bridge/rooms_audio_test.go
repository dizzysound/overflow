package bridge

import (
	"encoding/json"
	"testing"
)

func TestLiveRoomEventCarriesAudioStatus(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	snap := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}}
	m.Step(t0, snap)

	snap.Streams = []StreamStatus{{IP: "10.0.0.5", State: "streaming", Audio: "on"}}
	_, evs := m.Step(t0, snap)
	if len(evs) != 1 || evs[0].State != RoomLive || evs[0].Audio != "on" || evs[0].AudioReason != "" {
		t.Fatalf("audio on: evs = %+v", evs)
	}

	snap.Streams = []StreamStatus{{IP: "10.0.0.5", State: "streaming", Audio: "off", AudioReason: "audio stream ended: boom"}}
	_, evs = m.Step(t0, snap)
	if len(evs) != 1 || evs[0].Audio != "off" || evs[0].AudioReason != "audio stream ended: boom" {
		t.Fatalf("audio off: evs = %+v", evs)
	}

	_, evs = m.Step(t0, Snapshot{Devices: snap.Devices})
	if len(evs) != 1 || evs[0].State != RoomRetrying || evs[0].Audio != "" || evs[0].AudioReason != "" {
		t.Fatalf("after drop: evs = %+v", evs)
	}
}

func TestLiveRoomBeforeAudioDecidedOmitsIt(t *testing.T) {
	m := NewManager()
	m.SetRooms([]RoomSelection{{DeviceID: "A", AutoReconnect: true}})
	snap := Snapshot{Devices: []Device{dev("A", "10.0.0.5")}}
	m.Step(t0, snap)
	snap.Streams = []StreamStatus{{IP: "10.0.0.5", State: "streaming"}}
	_, evs := m.Step(t0, snap)
	if len(evs) != 1 || evs[0].State != RoomLive || evs[0].Audio != "" {
		t.Fatalf("evs = %+v", evs)
	}
}

func TestRoomEventJSONOmitsEmptyAudio(t *testing.T) {
	b, err := json.Marshal(RoomEvent{DeviceID: "A", State: RoomLive})
	if err != nil {
		t.Fatal(err)
	}
	if got, want := string(b), `{"device_id":"A","state":"live"}`; got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
	b, _ = json.Marshal(RoomEvent{DeviceID: "A", State: RoomLive, Audio: "off", AudioReason: "x"})
	if got, want := string(b), `{"device_id":"A","state":"live","audio":"off","audio_reason":"x"}`; got != want {
		t.Fatalf("got %s, want %s", got, want)
	}
}
