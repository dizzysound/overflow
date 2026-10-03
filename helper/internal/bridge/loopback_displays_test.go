package bridge_test

import (
	"context"
	"testing"
	"time"

	"doubletake/internal/airplay"
	"doubletake/internal/bridge"
)

// TestLoopbackSetDisplaysEmitsDisplayEvents drives a live receiver with the
// canonical set_displays command and checks that the helper reports state
// with "event":"display" lines only -- "room" is retired as an emitted name.
func TestLoopbackSetDisplaysEmitsDisplayEvents(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Nave", DeviceID: "AA:BB:CC:DD:EE:20",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	h.command(t, bridge.Command{Cmd: "set_displays", Displays: []bridge.RoomSelection{{DeviceID: dev.DeviceID, AutoReconnect: true}}})

	deadline := time.After(20 * time.Second)
	sawLive := false
	for !sawLive {
		select {
		case ev, ok := <-h.events:
			if !ok {
				t.Fatal("event stream closed waiting for display live")
			}
			if ev["event"] == "fatal" {
				t.Fatalf("fatal: %v", ev)
			}
			if ev["event"] == "room" {
				t.Fatalf("helper emitted a room event, want only display: %v", ev)
			}
			if ev["event"] == "display" && ev["device_id"] == dev.DeviceID && ev["state"] == "live" {
				sawLive = true
			}
		case <-deadline:
			h.timeout(t, "timed out waiting for display live event")
		}
	}
	waitMedia(t, srv)
}
