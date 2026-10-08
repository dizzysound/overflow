package airplay

import (
	"strings"
	"testing"
	"time"
)

// The backlog line must name the display: a 2026-09-28 report showed a
// backlog every 15-30 s across three receivers, with no way to tell which
// one was falling behind.
func TestBroadcastBacklogLogNamesTheSink(t *testing.T) {
	buf := captureDebugLog(t)
	frames := make(chan VideoAccessUnit)
	capture := &ScreenCapture{frames: &channelVideoAccessUnitReader{frames: frames}, waitCh: make(chan struct{})}
	broadcast := NewBroadcastCapture(capture)
	broadcast.SetDropToKeyframe(true)
	slow := broadcast.AddSink()
	slow.SetLabel("Gallery (10.20.0.185)")
	go func() { _ = broadcast.Run() }()
	t.Cleanup(func() { close(frames) })

	pFrame := func(tag byte) []byte { return append(append([]byte(nil), testP...), tag) }
	base := time.Now()
	for i, data := range [][]byte{testIDR, pFrame(1), pFrame(2), pFrame(3), pFrame(4)} {
		frames <- VideoAccessUnit{AnnexB: data, PTS: base.Add(time.Duration(i) * time.Second / 30)}
	}
	deadline := time.Now().Add(time.Second)
	for !strings.Contains(buf.String(), "sink backlog") && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	if !strings.Contains(buf.String(), "[BROADCAST] sink backlog (Gallery (10.20.0.185))") {
		t.Fatalf("backlog log does not name the sink:\n%s", buf.String())
	}
}
