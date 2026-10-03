package testfeed

import (
	"bytes"
	"context"
	"sync"
	"testing"
	"time"

	"doubletake/internal/bridge"
)

type lockedBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (l *lockedBuffer) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.b.Write(p)
}

func TestFeedVideoFPSStampsFramesByIndex(t *testing.T) {
	var out lockedBuffer
	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer cancel()
	if err := FeedWith(ctx, bridge.NewMessageWriter(&out), nil, Options{VideoFPS: 60000.0 / 1001}); err != nil {
		t.Fatal(err)
	}
	var captures []uint64
	r := bytes.NewReader(out.b.Bytes())
	for r.Len() > 0 {
		m, err := bridge.ReadMessage(r)
		if err != nil {
			t.Fatal(err)
		}
		if m.Type != bridge.MsgVideoAU {
			continue
		}
		media, err := bridge.DecodeMedia(m.Payload)
		if err != nil {
			t.Fatal(err)
		}
		captures = append(captures, media.CaptureNs)
	}
	// About 18 frames in 300 ms; allow for a slow test machine.
	if len(captures) < 10 {
		t.Fatalf("%d video frames in 300 ms at 59.94 fps", len(captures))
	}
	for i, c := range captures {
		want := uint64(i)*1001*uint64(time.Second)/60000 + 1
		if c+1 < want || c > want+1 {
			t.Fatalf("frame %d capture %d ns, want %d", i, c, want)
		}
	}
}
