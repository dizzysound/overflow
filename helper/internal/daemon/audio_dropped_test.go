package daemon

import (
	"sync/atomic"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

// countAudioDrops connects with the given lead and counts the audio frames the
// daemon drops as too late over the given time once streaming.
func countAudioDrops(t *testing.T, leadMs int, stall, over time.Duration) int64 {
	t.Helper()
	var drops atomic.Int64
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern,
		withAudioDropped(func(string) { drops.Add(1) }), withAudioStall(stall))
	resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, LatencyMs: leadMs, DeviceID: "AA:BB"})
	if !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
	time.Sleep(over)
	return drops.Load()
}

func TestAudioStallBeyondLeadCountsDroppedFrames(t *testing.T) {
	// A 150 ms audio stall against a 60 ms lead: the backlog arrives too late.
	if got := countAudioDrops(t, 60, 150*time.Millisecond, 5*time.Second); got == 0 {
		t.Fatal("no audio frames reported dropped after 150 ms stalls at a 60 ms lead")
	}
}

func TestSteadyAudioDropsNoFrames(t *testing.T) {
	// A generous 500 ms lead: at 60 ms a loaded CI runner's scheduling delays
	// alone made frames late (windows-latest, 2026-10-02: 2 dropped), which
	// the counter rightly reported.
	if got := countAudioDrops(t, 500, 0, 3*time.Second); got != 0 {
		t.Fatalf("%d audio frames reported dropped with a steady feed", got)
	}
}
