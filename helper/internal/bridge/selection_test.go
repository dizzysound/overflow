package bridge

import "testing"

func TestSanitizeRoomsDropsBadVolume(t *testing.T) {
	got := sanitizeRooms([]RoomSelection{
		{DeviceID: "A", AutoReconnect: true, VolumeDB: vol(-12)},
		{DeviceID: "B", AutoReconnect: true, VolumeDB: vol(3)},
	})
	if len(got) != 2 {
		t.Fatalf("got %d rooms, want 2 (a bad field never drops the room)", len(got))
	}
	if got[0].VolumeDB == nil || *got[0].VolumeDB != -12 {
		t.Fatalf("room A volume = %v", got[0].VolumeDB)
	}
	if got[1].VolumeDB != nil || !got[1].AutoReconnect {
		t.Fatalf("room B = %+v, want volume cleared and the rest kept", got[1])
	}
}

func TestSanitizeRoomsKeepsAudio(t *testing.T) {
	off := false
	got := sanitizeRooms([]RoomSelection{
		{DeviceID: "A", AutoReconnect: true, Audio: &off},
		{DeviceID: "B", AutoReconnect: true},
	})
	if got[0].Audio == nil || *got[0].Audio {
		t.Fatalf("room A audio = %v, want false pointer", got[0].Audio)
	}
	if got[1].Audio != nil {
		t.Fatalf("room B audio = %v, want nil", *got[1].Audio)
	}
}

func TestSanitizeRoomsKeepsWifiTolerant(t *testing.T) {
	on := true
	got := sanitizeRooms([]RoomSelection{
		{DeviceID: "A", AutoReconnect: true, WifiTolerant: &on},
		{DeviceID: "B", AutoReconnect: true},
	})
	if got[0].WifiTolerant == nil || !*got[0].WifiTolerant {
		t.Fatalf("room A wifi_tolerant = %v, want true pointer", got[0].WifiTolerant)
	}
	if got[1].WifiTolerant != nil {
		t.Fatalf("room B wifi_tolerant = %v, want nil", *got[1].WifiTolerant)
	}
}

func TestSanitizeRoomsManualAddress(t *testing.T) {
	got := sanitizeRooms([]RoomSelection{
		{DeviceID: "A", IP: "10.20.0.178", Port: 7000},
		{DeviceID: "B", IP: "studio.local"},
		{DeviceID: "C", IP: "10.20.0.164", Port: 70000},
		{DeviceID: "D", Port: 7000},
		{DeviceID: "E", IP: "fe80::1"},
	})
	if got[0].IP != "10.20.0.178" || got[0].Port != 7000 {
		t.Fatalf("valid address changed: %+v", got[0])
	}
	for _, i := range []int{1, 2, 3} {
		if got[i].IP != "" || got[i].Port != 0 {
			t.Fatalf("room %s kept an invalid address: %+v", got[i].DeviceID, got[i])
		}
	}
	if got[4].IP != "fe80::1" {
		t.Fatalf("IPv6 literal rejected: %+v", got[4])
	}
}

func TestSanitizeRoomsDropsUnknownAudioFormat(t *testing.T) {
	got := sanitizeRooms([]RoomSelection{
		{DeviceID: "A", AudioFormat: "aac-eld"},
		{DeviceID: "B", AudioFormat: "opus"},
	})
	if got[0].AudioFormat != "aac-eld" || got[1].AudioFormat != "" {
		t.Fatalf("audio_format = %q, %q; want aac-eld kept and opus dropped", got[0].AudioFormat, got[1].AudioFormat)
	}
}
