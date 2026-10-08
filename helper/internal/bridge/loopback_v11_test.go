package bridge_test

import (
	"context"
	"testing"
	"time"

	"doubletake/internal/airplay"
	"doubletake/internal/bridge"
)

// waitParameterRequests waits until the receiver has seen at least min
// SET_PARAMETER requests (the helper sends volume twice at session start and
// once per set_volume).
func waitParameterRequests(t *testing.T, srv *airplay.ReceiverServer, min uint64) {
	t.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if srv.Stats().ParameterRequests >= min {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("ParameterRequests = %d, want at least %d", srv.Stats().ParameterRequests, min)
}

func TestLoopbackRoomVolume(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Nave", DeviceID: "AA:BB:CC:DD:EE:11",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	start := -12.0
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{{DeviceID: dev.DeviceID, AutoReconnect: true, VolumeDB: &start}}})
	h.waitRoom(t, dev.DeviceID, "live", 20*time.Second)
	waitParameterRequests(t, srv, 2)

	live := -20.0
	h.command(t, bridge.Command{Cmd: "set_volume", DeviceID: dev.DeviceID, VolumeDB: &live})
	waitParameterRequests(t, srv, 3)
}

// waitRoomWhere returns the first room event for id that satisfies pred.
func (h *harness) waitRoomWhere(t *testing.T, id string, pred func(map[string]any) bool, timeout time.Duration) map[string]any {
	t.Helper()
	deadline := time.After(timeout)
	for {
		select {
		case ev, ok := <-h.events:
			if !ok {
				t.Fatalf("event stream closed waiting for room %s", id)
			}
			if ev["event"] == "fatal" {
				t.Fatalf("fatal: %v", ev)
			}
			if ev["event"] == "display" && ev["device_id"] == id && pred(ev) {
				return ev
			}
		case <-deadline:
			h.timeout(t, "timed out waiting for a matching room event for %s", id)
		}
	}
}

func liveWithAudio(ev map[string]any) bool { return ev["state"] == "live" && ev["audio"] != nil }

func TestLoopbackLiveRoomReportsAudioOn(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	_, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Lobby", DeviceID: "AA:BB:CC:DD:EE:12",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{{DeviceID: dev.DeviceID, AutoReconnect: true}}})
	ev := h.waitRoomWhere(t, dev.DeviceID, liveWithAudio, 20*time.Second)
	if ev["audio"] != "on" {
		t.Fatalf("audio = %v, want on", ev["audio"])
	}
	if _, ok := ev["audio_reason"]; ok {
		t.Fatalf("audio_reason present while audio is on: %v", ev)
	}
}

func TestLoopbackAACELDOnlyRoomReportsAudioOff(t *testing.T) {
	if capture, err := airplay.NewExternalAudioSource().Subscribe(airplay.AudioCodecAACELD); err == nil {
		capture.Stop()
		t.Skip("this build can encode AAC-ELD; the video-only degrade does not apply")
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	_, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileAppleTV3, Auth: airplay.ReceiverAuthNone,
		Name: "Gallery", DeviceID: "AA:BB:CC:DD:EE:14", LegacyAudioOutputFormats: 0x1000000,
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{{DeviceID: dev.DeviceID, AutoReconnect: true}}})
	ev := h.waitRoomWhere(t, dev.DeviceID, liveWithAudio, 20*time.Second)
	reason, _ := ev["audio_reason"].(string)
	if ev["audio"] != "off" || reason == "" {
		t.Fatalf("event = %v, want audio off with a reason", ev)
	}
}

func TestLoopbackDisplayAudioOff(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Studio", DeviceID: "AA:BB:CC:DD:EE:16",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	noAudio := false
	h.command(t, bridge.Command{Cmd: "set_displays", Displays: []bridge.RoomSelection{
		{DeviceID: dev.DeviceID, AutoReconnect: true, Audio: &noAudio},
	}})
	ev := h.waitRoomWhere(t, dev.DeviceID, liveWithAudio, 20*time.Second)
	reason, _ := ev["audio_reason"].(string)
	if ev["audio"] != "off" || reason != "turned off for this display" {
		t.Fatalf("event = %v, want audio off with reason %q", ev, "turned off for this display")
	}
	// Give the stream a moment to run, then confirm no audio ever reached the
	// receiver: this display's session never negotiated/streamed audio, unlike
	// the AAC-ELD-unavailable case, which still negotiates the audio session.
	time.Sleep(200 * time.Millisecond)
	if n := srv.Stats().AudioPackets; n != 0 {
		t.Fatalf("AudioPackets = %d, want 0 (video-only display)", n)
	}
}

func TestLoopbackRestartReconnects(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Library", DeviceID: "AA:BB:CC:DD:EE:13",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{{DeviceID: dev.DeviceID, AutoReconnect: true}}})
	h.waitRoom(t, dev.DeviceID, "live", 20*time.Second)
	before := srv.Stats().SetupRequests

	h.command(t, bridge.Command{Cmd: "restart", DeviceID: dev.DeviceID})
	h.waitRoom(t, dev.DeviceID, "connecting", 10*time.Second)
	h.waitRoom(t, dev.DeviceID, "live", 20*time.Second)
	if after := srv.Stats().SetupRequests; after <= before {
		t.Fatalf("SetupRequests %d -> %d: restart did not set up a new session", before, after)
	}
}

// TestLoopbackManualAddressRoom selects a receiver that is not in the device
// list (no ExtraDevices; the in-process receiver does not advertise mDNS) by
// its address alone.
func TestLoopbackManualAddressRoom(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Atrium", DeviceID: "AA:BB:CC:DD:EE:15",
	})
	h := startHarness(t, ctx, nil)
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{
		{DeviceID: dev.DeviceID, AutoReconnect: true, IP: dev.IP, Port: dev.Port},
	}})
	h.waitRoom(t, dev.DeviceID, "live", 20*time.Second)
	waitMedia(t, srv)
}
