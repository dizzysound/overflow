package bridge_test

import (
	"context"
	"reflect"
	"testing"
	"time"

	"doubletake/internal/airplay"
	"doubletake/internal/bridge"
)

// waitEvent returns the first event (any kind) that satisfies pred, failing on
// timeout or a fatal event.
func (h *harness) waitEvent(t *testing.T, pred func(map[string]any) bool, timeout time.Duration) map[string]any {
	t.Helper()
	deadline := time.After(timeout)
	for {
		select {
		case ev, ok := <-h.events:
			if !ok {
				t.Fatal("event stream closed waiting for an event")
			}
			if ev["event"] == "fatal" {
				t.Fatalf("fatal: %v", ev)
			}
			if pred(ev) {
				return ev
			}
		case <-deadline:
			h.timeout(t, "timed out waiting for a matching event")
		}
	}
}

func TestLoopbackSetLeadAndReadyCapability(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	_, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Nave", DeviceID: "AA:BB:CC:DD:EE:41",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})

	// The harness queues every event, so ready is first in line.
	ready := h.waitEvent(t, func(ev map[string]any) bool { return ev["event"] == "ready" }, 10*time.Second)
	if caps, _ := ready["capabilities"].([]any); !reflect.DeepEqual(caps, []any{"live_lead"}) {
		t.Fatalf("ready capabilities = %v, want [live_lead]", ready["capabilities"])
	}

	// A large lead: CI's Windows runners drop audio at small ones.
	lead, headroom := 520, 60
	h.command(t, bridge.Command{Cmd: "set_displays", Displays: []bridge.RoomSelection{{
		DeviceID: dev.DeviceID, AutoReconnect: true, LatencyMs: &lead, LeadHeadroomMs: &headroom,
	}}})
	h.waitRoom(t, dev.DeviceID, "live", 20*time.Second)

	target := 500
	h.command(t, bridge.Command{Cmd: "set_lead", DeviceID: dev.DeviceID, LeadMs: &target})
	ev := h.waitEvent(t, func(ev map[string]any) bool {
		return ev["event"] == "delivery" && ev["device_id"] == dev.DeviceID && ev["lead_target_ms"] == float64(500)
	}, 15*time.Second)
	if ev["lead_ceiling_ms"] != float64(580) {
		t.Fatalf("lead_ceiling_ms = %v, want 580 (event %v)", ev["lead_ceiling_ms"], ev)
	}
}
