package main

import (
	"strings"
	"testing"
)

func TestParseRooms(t *testing.T) {
	sel, err := parseRooms("AA:BB:CC:DD:EE:01, 0C:FB:30:58:DF:2E@10.20.0.178:7000", []string{"aa:bb:cc:dd:ee:01=-12"}, nil, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(sel) != 2 {
		t.Fatalf("got %d rooms", len(sel))
	}
	if sel[0].DeviceID != "AA:BB:CC:DD:EE:01" || !sel[0].AutoReconnect || sel[0].VolumeDB == nil || *sel[0].VolumeDB != -12 || sel[0].IP != "" {
		t.Fatalf("room 0 = %+v", sel[0])
	}
	if sel[1].DeviceID != "0C:FB:30:58:DF:2E" || sel[1].IP != "10.20.0.178" || sel[1].Port != 7000 || sel[1].VolumeDB != nil {
		t.Fatalf("room 1 = %+v", sel[1])
	}
}

func TestParseRoomsEmpty(t *testing.T) {
	sel, err := parseRooms("", nil, nil, nil, nil)
	if err != nil || len(sel) != 0 {
		t.Fatalf("sel=%+v err=%v", sel, err)
	}
}

func TestParseRoomsAudioOff(t *testing.T) {
	sel, err := parseRooms("AA:BB:CC:DD:EE:01,AA:BB:CC:DD:EE:02", nil, []string{"aa:bb:cc:dd:ee:01"}, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(sel) != 2 {
		t.Fatalf("got %d rooms", len(sel))
	}
	if sel[0].Audio == nil || *sel[0].Audio {
		t.Fatalf("room 0 audio = %v, want false pointer", sel[0].Audio)
	}
	if sel[1].Audio != nil {
		t.Fatalf("room 1 audio = %v, want nil", *sel[1].Audio)
	}
}

func TestParseRoomsWifiTolerant(t *testing.T) {
	sel, err := parseRooms("AA:BB:CC:DD:EE:01,AA:BB:CC:DD:EE:02", nil, nil, []string{"aa:bb:cc:dd:ee:02"}, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(sel) != 2 {
		t.Fatalf("got %d rooms", len(sel))
	}
	if sel[0].WifiTolerant != nil {
		t.Fatalf("room 0 wifi_tolerant = %v, want nil", *sel[0].WifiTolerant)
	}
	if sel[1].WifiTolerant == nil || !*sel[1].WifiTolerant {
		t.Fatalf("room 1 wifi_tolerant = %v, want true pointer", sel[1].WifiTolerant)
	}
}

func TestParseRoomsRejectsBadInput(t *testing.T) {
	for _, tc := range []struct {
		rooms        string
		volumes      []string
		audioOff     []string
		wifiTolerant []string
		want         string
	}{
		{"A", []string{"A"}, nil, nil, "DEVICEID=DB"},
		{"A", []string{"A=loud"}, nil, nil, "-room-volume"},
		{"A", []string{"A=5"}, nil, nil, "-30 to 0"},
		{"A@host.local", nil, nil, nil, "not an IP address"},
		{"A", nil, []string{"  "}, nil, "-display-audio-off"},
		{"A", nil, nil, []string{"  "}, "-display-wifi-tolerant"},
	} {
		if _, err := parseRooms(tc.rooms, tc.volumes, tc.audioOff, tc.wifiTolerant, nil); err == nil || !strings.Contains(err.Error(), tc.want) {
			t.Errorf("parseRooms(%q, %q, %q, %q) error = %v, want one containing %q", tc.rooms, tc.volumes, tc.audioOff, tc.wifiTolerant, err, tc.want)
		}
	}
}

func TestParseTypedCommand(t *testing.T) {
	c, err := parseTypedCommand("/restart 0C:FB:30:58:DF:2E")
	if err != nil || c.Cmd != "restart" || c.DeviceID != "0C:FB:30:58:DF:2E" {
		t.Fatalf("restart: %+v %v", c, err)
	}
	c, err = parseTypedCommand("/volume 0C:FB:30:58:DF:2E -15.5")
	if err != nil || c.Cmd != "set_volume" || c.DeviceID != "0C:FB:30:58:DF:2E" || c.VolumeDB == nil || *c.VolumeDB != -15.5 {
		t.Fatalf("volume: %+v %v", c, err)
	}
	for _, bad := range []string{"/", "/restart", "/volume A", "/volume A x", "/volume A 3", "/mute A"} {
		if _, err := parseTypedCommand(bad); err == nil {
			t.Errorf("parseTypedCommand(%q) = nil error", bad)
		}
	}
}

func TestParseRoomsLatency(t *testing.T) {
	sel, err := parseRooms("AA:BB", nil, nil, nil, []string{"aa:bb=95"})
	if err != nil || len(sel) != 1 || sel[0].LatencyMs == nil || *sel[0].LatencyMs != 95 {
		t.Fatalf("sel = %+v, err = %v", sel, err)
	}
	if _, err := parseRooms("AA:BB", nil, nil, nil, []string{"AA:BB=0"}); err == nil {
		t.Fatal("want error for 0 ms")
	}
}

func TestParseVideoFPS(t *testing.T) {
	for _, tc := range []struct {
		in   string
		want float64
		ok   bool
	}{
		{"", 0, true},
		{"30", 30, true},
		{"60", 60, true},
		{"59.94", 60000.0 / 1001, true},
		{"29.97", 30000.0 / 1001, true},
		{"0", 0, false},
		{"-60", 0, false},
		{"121", 0, false},
		{"fast", 0, false},
	} {
		got, err := parseVideoFPS(tc.in)
		if (err == nil) != tc.ok || got != tc.want {
			t.Errorf("parseVideoFPS(%q) = %v, %v; want %v, ok=%t", tc.in, got, err, tc.want, tc.ok)
		}
	}
}

func TestParseTypedLead(t *testing.T) {
	c, err := parseTypedCommand("/lead 0C:FB:30:58:DF:2E 70")
	if err != nil || c.Cmd != "set_lead" || c.DeviceID != "0C:FB:30:58:DF:2E" || c.LeadMs == nil || *c.LeadMs != 70 {
		t.Fatalf("got %+v, %v", c, err)
	}
	for _, bad := range []string{"/lead", "/lead AA", "/lead AA x", "/lead AA 0"} {
		if _, err := parseTypedCommand(bad); err == nil {
			t.Fatalf("%q accepted", bad)
		}
	}
}

func TestParseRoomsLeadHeadroom(t *testing.T) {
	sel, err := parseRoomsWithHeadroom("AA:BB:CC:DD:EE:01", nil, nil, nil, []string{"aa:bb:cc:dd:ee:01=70"}, []string{"aa:bb:cc:dd:ee:01=60"})
	if err != nil {
		t.Fatal(err)
	}
	if sel[0].LeadHeadroomMs == nil || *sel[0].LeadHeadroomMs != 60 || sel[0].LatencyMs == nil || *sel[0].LatencyMs != 70 {
		t.Fatalf("room %+v", sel[0])
	}
	if _, err := parseRoomsWithHeadroom("AA:BB:CC:DD:EE:01", nil, nil, nil, nil, []string{"aa:bb:cc:dd:ee:01=600"}); err == nil {
		t.Fatal("headroom 600 accepted")
	}
}
