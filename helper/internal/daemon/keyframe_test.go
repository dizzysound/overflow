package daemon

import (
	"sync"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

func TestAddSinkRequestsJoinKeyframe(t *testing.T) {
	var mu sync.Mutex
	var got []string
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern, withKeyframeRequests(func(id, reason string) {
		mu.Lock()
		got = append(got, id+"/"+reason)
		mu.Unlock()
	}))
	if resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, DeviceID: "AA:BB"}); !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
	deadline := time.Now().Add(2 * time.Second)
	for {
		mu.Lock()
		n := len(got)
		mu.Unlock()
		if n >= 2 || time.Now().After(deadline) {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	// The embedded receiver also sends forceKeyFrame at stream start, so the
	// join request may follow a "receiver" one; only its presence is asserted.
	found := false
	for _, r := range got {
		found = found || r == "AA:BB/join"
	}
	if !found {
		t.Fatalf("keyframe requests = %v, want AA:BB/join among them", got)
	}
}
