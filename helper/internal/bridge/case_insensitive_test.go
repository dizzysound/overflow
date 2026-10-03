package bridge_test

import (
	"context"
	"strings"
	"testing"
	"time"

	"doubletake/internal/airplay"
	"doubletake/internal/bridge"
)

// These tests cover the controller ruling that device IDs are
// case-insensitive across all sources: mDNS (which reports some receivers,
// e.g. the Android UxPlay port, in lowercase), the -device flag and
// set_rooms/command device_id fields (any case), and the daemon's own /info
// deviceID. The bridge normalizes every device ID to uppercase at its
// boundaries so a receiver is never split into two unrelated rooms.

// TestCaseInsensitiveRoomSelectionResolvesToOneRoom selects a receiver whose
// discovered (ExtraDevices, standing in for mDNS) device ID is lowercase using
// an uppercase set_rooms entry. Before normalization the room's key (uppercase,
// from set_rooms) never matched the discovered device (lowercase), so the room
// stayed offline forever.
func TestCaseInsensitiveRoomSelectionResolvesToOneRoom(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	_, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Uxplay Room", DeviceID: "aa:bb:cc:dd:ee:20",
	})
	if dev.DeviceID != strings.ToLower(dev.DeviceID) {
		t.Fatalf("test setup: dev.DeviceID = %q, want lowercase", dev.DeviceID)
	}
	h := startHarness(t, ctx, []bridge.Device{dev})
	upper := strings.ToUpper(dev.DeviceID)
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{{DeviceID: upper, AutoReconnect: true}}})
	h.waitRoom(t, upper, "live", 20*time.Second)
}

// TestCaseInsensitiveCommandReachesRoom selects a room by its uppercase device
// ID, then sends a restart command using the lowercase device_id (as reported
// by mDNS/the receiver) and confirms it reaches the same, uppercase-keyed room.
func TestCaseInsensitiveCommandReachesRoom(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Uxplay Restart Room", DeviceID: "aa:bb:cc:dd:ee:21",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	upper := strings.ToUpper(dev.DeviceID)
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{{DeviceID: upper, AutoReconnect: true}}})
	h.waitRoom(t, upper, "live", 20*time.Second)
	before := srv.Stats().SetupRequests

	// Lowercase device_id, exactly as the receiver/mDNS reports it.
	h.command(t, bridge.Command{Cmd: "restart", DeviceID: dev.DeviceID})
	h.waitRoom(t, upper, "connecting", 10*time.Second)
	h.waitRoom(t, upper, "live", 20*time.Second)
	if after := srv.Stats().SetupRequests; after <= before {
		t.Fatalf("SetupRequests %d -> %d: lowercase restart did not reach the room", before, after)
	}
}

// TestDevicesEventsAreUppercase confirms the helper always reports device IDs
// in uppercase on the devices event, whatever case discovery (or ExtraDevices,
// standing in for it) supplied.
func TestDevicesEventsAreUppercase(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	_, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Uxplay Devices Room", DeviceID: "aa:bb:cc:dd:ee:22",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	deadline := time.After(10 * time.Second)
	for {
		select {
		case ev, ok := <-h.events:
			if !ok {
				t.Fatal("event stream closed before a devices event arrived")
			}
			if ev["event"] != "devices" {
				continue
			}
			ds, ok := ev["devices"].([]any)
			if !ok || len(ds) == 0 {
				continue
			}
			found := false
			for _, raw := range ds {
				d, ok := raw.(map[string]any)
				if !ok {
					continue
				}
				id, _ := d["device_id"].(string)
				if strings.EqualFold(id, dev.DeviceID) {
					found = true
					if id != strings.ToUpper(dev.DeviceID) {
						t.Fatalf("devices event device_id = %q, want %q", id, strings.ToUpper(dev.DeviceID))
					}
				}
			}
			if found {
				return
			}
		case <-deadline:
			t.Fatal("timed out waiting for a devices event listing the receiver")
		}
	}
}
