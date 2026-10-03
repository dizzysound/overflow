package daemon

import (
	"testing"
	"time"

	"doubletake/internal/airplay"
)

// streamSinkBudget returns the relay budget of the only stream's sink.
func streamSinkBudget(t *testing.T, d *Daemon) time.Duration {
	t.Helper()
	d.mu.Lock()
	defer d.mu.Unlock()
	if len(d.streams) != 1 {
		t.Fatalf("%d streams, want 1", len(d.streams))
	}
	for _, entry := range d.streams {
		if entry.sink == nil {
			t.Fatal("streaming entry has no sink")
		}
		return entry.sink.MaxFrameQueueDuration()
	}
	return 0
}

func TestConnectWifiTolerantRaisesThatSinksBudget(t *testing.T) {
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, WifiTolerant: true})
	if !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
	if got := streamSinkBudget(t, d); got != airplay.WifiTolerantFrameQueueDuration {
		t.Fatalf("sink budget = %v, want %v", got, airplay.WifiTolerantFrameQueueDuration)
	}
}

func TestConnectWithoutWifiTolerantKeepsDefaultBudget(t *testing.T) {
	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	waitStreaming(t, d, server)
	if got := streamSinkBudget(t, d); got != 67*time.Millisecond {
		t.Fatalf("sink budget = %v, want the 67ms default", got)
	}
}
