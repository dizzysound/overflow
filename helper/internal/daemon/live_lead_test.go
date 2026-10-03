package daemon

import (
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

func TestSetLeadSlidesALiveSessionWithoutDroppingAudio(t *testing.T) {
	if err := airplay.SetLeadSlidePPM(5000); err != nil { // 5 ms per second keeps the test short
		t.Fatal(err)
	}
	defer airplay.SetLeadSlidePPM(300)
	var mu sync.Mutex
	var last airplay.LeadState
	// Leads of 500 ms and up: CI runners drop frames at 60 ms even with a steady
	// feed (8b40304), and this test asserts zero drops.
	var drops atomic.Int64
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern,
		withAudioDropped(func(string) { drops.Add(1) }),
		withFrameDelivered(func(_ string, lead airplay.LeadState, _ time.Duration) {
			mu.Lock()
			last = lead
			mu.Unlock()
		}))
	if resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, LatencyMs: 520, LeadHeadroomMs: 60, DeviceID: "AA:BB"}); !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
	if resp := d.HandleRequest(Request{Cmd: "set_lead", Target: addr.IP.String(), LatencyMs: 500}); !resp.OK {
		t.Fatalf("set_lead: %+v", resp)
	}
	// 20 ms at 5000 ppm takes 4 s; poll instead of sleeping a fixed time.
	var got airplay.LeadState
	for deadline := time.Now().Add(10 * time.Second); time.Now().Before(deadline); time.Sleep(100 * time.Millisecond) {
		mu.Lock()
		got = last
		mu.Unlock()
		if got.Effective == 500*time.Millisecond && got.Target == 500*time.Millisecond {
			break
		}
	}
	if got.Ceiling != 580*time.Millisecond || got.Target != 500*time.Millisecond || got.Effective != 500*time.Millisecond {
		t.Fatalf("lead state %+v, want effective = target 500ms under a 580ms ceiling", got)
	}
	if n := drops.Load(); n != 0 {
		t.Fatalf("%d audio frames dropped while sliding from 520 to 500 ms", n)
	}
}

func TestSetLeadOnAFixedOrUnknownSessionFails(t *testing.T) {
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	if resp := d.HandleRequest(Request{Cmd: "set_lead", Target: "10.9.9.9", LatencyMs: 80}); resp.OK {
		t.Fatal("set_lead to an unknown target succeeded")
	}
	if resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, LatencyMs: 120, DeviceID: "AA:BB"}); !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
	if resp := d.HandleRequest(Request{Cmd: "set_lead", Target: addr.IP.String(), LatencyMs: 80}); resp.OK {
		t.Fatal("set_lead on a fixed-lead session succeeded")
	}
	if resp := d.HandleRequest(Request{Cmd: "set_lead", Target: addr.IP.String()}); resp.OK {
		t.Fatal("set_lead without latency_ms succeeded")
	}
}
