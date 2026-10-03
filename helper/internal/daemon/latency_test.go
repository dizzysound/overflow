package daemon

import (
	"testing"
	"time"

	"doubletake/internal/airplay"
)

func TestConnectLatencySetsLeadDerivedBudget(t *testing.T) {
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, LatencyMs: 150, DeviceID: "AA:BB"})
	if !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
	if got := streamSinkBudget(t, d); got != 120*time.Millisecond {
		t.Fatalf("sink budget = %v, want 120ms (lead 150 - 30)", got)
	}
}

func TestConnectLatencyOverridesWifiBudget(t *testing.T) {
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, LatencyMs: 100, WifiTolerant: true})
	if !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
	if got := streamSinkBudget(t, d); got != 70*time.Millisecond {
		t.Fatalf("sink budget = %v, want 70ms: an explicit lead replaces the 250ms Wi-Fi budget", got)
	}
}

func TestConnectWithoutLatencyKeepsDefaultBudget(t *testing.T) {
	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	waitStreaming(t, d, server)
	if got := streamSinkBudget(t, d); got != 67*time.Millisecond {
		t.Fatalf("sink budget = %v, want the 67ms default", got)
	}
}

func TestConnectKeepsPluginDisplayID(t *testing.T) {
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, LatencyMs: 100, DeviceID: "AA:BB"})
	if !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
	d.mu.Lock()
	defer d.mu.Unlock()
	if len(d.streams) != 1 {
		t.Fatalf("%d streams, want 1", len(d.streams))
	}
	for _, entry := range d.streams {
		if entry.displayID != "AA:BB" {
			t.Fatalf("displayID = %q, want the plugin's \"AA:BB\" (not overwritten by the receiver's /info ID)", entry.displayID)
		}
	}
}
