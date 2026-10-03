package bridge

import (
	"encoding/json"
	"testing"
)

func TestSetRoomsVolumeDecodes(t *testing.T) {
	var c Command
	in := `{"cmd":"set_rooms","rooms":[{"device_id":"A","auto_reconnect":true,"volume_db":0},{"device_id":"B","auto_reconnect":true}]}`
	if err := json.Unmarshal([]byte(in), &c); err != nil {
		t.Fatal(err)
	}
	if c.Rooms[0].VolumeDB == nil || *c.Rooms[0].VolumeDB != 0 {
		t.Fatalf("0 dB must decode as a level, not as absent: %+v", c.Rooms[0])
	}
	if c.Rooms[1].VolumeDB != nil {
		t.Fatalf("absent volume_db decoded as %v, want nil", *c.Rooms[1].VolumeDB)
	}
}

func TestSetRoomsManualAddressDecodes(t *testing.T) {
	var c Command
	in := `{"cmd":"set_rooms","rooms":[{"device_id":"A","auto_reconnect":true,"ip":"10.20.0.178","port":7000}]}`
	if err := json.Unmarshal([]byte(in), &c); err != nil {
		t.Fatal(err)
	}
	if c.Rooms[0].IP != "10.20.0.178" || c.Rooms[0].Port != 7000 {
		t.Fatalf("decoded %+v", c.Rooms[0])
	}
}

func TestSetDisplaysAudioDecodes(t *testing.T) {
	var c Command
	in := `{"cmd":"set_displays","displays":[{"device_id":"A","auto_reconnect":true,"audio":false},{"device_id":"B","auto_reconnect":true}]}`
	if err := json.Unmarshal([]byte(in), &c); err != nil {
		t.Fatal(err)
	}
	if c.Displays[0].Audio == nil || *c.Displays[0].Audio {
		t.Fatalf("audio:false must decode as a false pointer: %+v", c.Displays[0])
	}
	if c.Displays[1].Audio != nil {
		t.Fatalf("absent audio decoded as %v, want nil", *c.Displays[1].Audio)
	}
}

func TestSetDisplaysWifiTolerantDecodes(t *testing.T) {
	var c Command
	in := `{"cmd":"set_displays","displays":[{"device_id":"A","auto_reconnect":true,"wifi_tolerant":true},{"device_id":"B","auto_reconnect":true}]}`
	if err := json.Unmarshal([]byte(in), &c); err != nil {
		t.Fatal(err)
	}
	if c.Displays[0].WifiTolerant == nil || !*c.Displays[0].WifiTolerant {
		t.Fatalf("wifi_tolerant:true must decode as a true pointer: %+v", c.Displays[0])
	}
	if c.Displays[1].WifiTolerant != nil {
		t.Fatalf("absent wifi_tolerant decoded as %v, want nil", *c.Displays[1].WifiTolerant)
	}
}

func TestSetVolumeDecodes(t *testing.T) {
	var c Command
	if err := json.Unmarshal([]byte(`{"cmd":"set_volume","device_id":"A","volume_db":-12}`), &c); err != nil {
		t.Fatal(err)
	}
	if c.Cmd != "set_volume" || c.DeviceID != "A" || c.VolumeDB == nil || *c.VolumeDB != -12 {
		t.Fatalf("decoded %+v", c)
	}
}
