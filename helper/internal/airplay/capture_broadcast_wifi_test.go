package airplay

import (
	"bytes"
	"errors"
	"io"
	"testing"
	"time"
)

// stallAndDrain feeds nine access units at 60 fps (a nominal 150 ms) to a
// shared drop-to-keyframe sink whose reader does not read until the source
// ends, then drains it. budget 0 keeps the default relay budget.
func stallAndDrain(t *testing.T, budget time.Duration) (got [][]byte, dropped uint64) {
	t.Helper()
	frames := make(chan VideoAccessUnit)
	capture := &ScreenCapture{frames: &channelVideoAccessUnitReader{frames: frames}, waitCh: make(chan struct{})}
	broadcast := NewBroadcastCaptureWithFrameRate(capture, 60)
	broadcast.SetDropToKeyframe(true)
	slow := broadcast.AddSink()
	if budget > 0 {
		slow.SetMaxFrameQueueDuration(budget)
	}
	runDone := make(chan error, 1)
	go func() { runDone <- broadcast.Run() }()

	base := time.Now()
	for i := 0; i < 9; i++ {
		data := append(append([]byte(nil), testP...), byte(i))
		if i == 0 {
			data = testIDR
		}
		frames <- VideoAccessUnit{AnnexB: data, PTS: base.Add(time.Duration(i) * time.Second / 60)}
	}
	close(frames)
	for {
		frame, err := slow.ReadVideoAccessUnit()
		if len(frame.AnnexB) > 0 {
			got = append(got, frame.AnnexB)
		}
		if err != nil {
			break
		}
	}
	select {
	case err := <-runDone:
		if !errors.Is(err, io.EOF) {
			t.Fatalf("broadcast run = %v, want EOF", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("broadcast did not finish")
	}
	return got, slow.DroppedFrames()
}

// The default 67 ms budget holds four 60 fps frames, so a 150 ms Wi-Fi stall
// sheds the backlog and freezes the display until the next IDR.
func TestBroadcastDefaultBudgetDropsA150msStallToKeyframe(t *testing.T) {
	got, dropped := stallAndDrain(t, 0)
	if dropped == 0 {
		t.Fatalf("DroppedFrames = 0 with the default budget; want the stall dropped to keyframe (got %d AUs)", len(got))
	}
}

// With the Wi-Fi tolerant budget the same stall is absorbed: every access
// unit is delivered, in order, and nothing is dropped.
func TestBroadcastWifiTolerantBudgetAbsorbsA150msStall(t *testing.T) {
	got, dropped := stallAndDrain(t, WifiTolerantFrameQueueDuration)
	if dropped != 0 {
		t.Fatalf("DroppedFrames = %d with the Wi-Fi tolerant budget, want 0", dropped)
	}
	if len(got) != 9 || !bytes.Equal(got[0], testIDR) {
		t.Fatalf("delivered %d AUs (first %x), want all 9 starting with the IDR", len(got), got)
	}
	for i := 1; i < len(got); i++ {
		if got[i][len(got[i])-1] != byte(i) {
			t.Fatalf("AU %d = %x, out of order", i, got[i])
		}
	}
}

func TestBroadcastSinkSetMaxFrameQueueDuration(t *testing.T) {
	s := newBroadcastSink(nil)
	if d := s.MaxFrameQueueDuration(); d != ordinaryScreenFrameQueueDuration {
		t.Fatalf("default budget = %v, want %v", d, ordinaryScreenFrameQueueDuration)
	}
	s.SetMaxFrameQueueDuration(WifiTolerantFrameQueueDuration)
	if d := s.MaxFrameQueueDuration(); d != 250*time.Millisecond {
		t.Fatalf("budget = %v, want 250ms", d)
	}
}
