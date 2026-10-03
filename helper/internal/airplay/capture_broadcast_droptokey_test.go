package airplay

import (
	"bytes"
	"errors"
	"io"
	"testing"
	"time"
)

// With drop-to-keyframe enabled, a stalled shared sink sheds its backlog and
// resumes at the next IDR instead of being detached, while its peer receives
// every frame.
func TestBroadcastDropToKeyframeResyncsSlowSinkAtIDR(t *testing.T) {
	frames := make(chan VideoAccessUnit)
	capture := &ScreenCapture{
		frames: &channelVideoAccessUnitReader{frames: frames},
		waitCh: make(chan struct{}),
	}
	broadcast := NewBroadcastCapture(capture)
	broadcast.SetDropToKeyframe(true)
	slow := broadcast.AddSink()
	healthy := broadcast.AddSink()
	runDone := make(chan error, 1)
	go func() { runDone <- broadcast.Run() }()

	healthyFrames := make(chan VideoAccessUnit, 16)
	go func() {
		for {
			frame, err := healthy.ReadVideoAccessUnit()
			if len(frame.AnnexB) > 0 {
				healthyFrames <- frame
			}
			if err != nil {
				return
			}
		}
	}()

	idr2 := append(append([]byte(nil), testIDR...), 0x02)
	pFrame := func(tag byte) []byte { return append(append([]byte(nil), testP...), tag) }
	// At 30 fps the 67 ms nominal budget admits two queued AUs; the third
	// overflows. The slow sink never reads during this sequence.
	sequence := [][]byte{testIDR, pFrame(1), pFrame(2), pFrame(3), pFrame(4), idr2, pFrame(5)}
	base := time.Now()
	for i, data := range sequence {
		want := VideoAccessUnit{AnnexB: data, PTS: base.Add(time.Duration(i) * time.Second / 30)}
		frames <- want
		select {
		case got := <-healthyFrames:
			if !bytes.Equal(got.AnnexB, want.AnnexB) {
				t.Fatalf("healthy frame %d = %x, want %x", i, got.AnnexB, want.AnnexB)
			}
		case <-time.After(time.Second):
			t.Fatalf("healthy sink stalled at frame %d", i)
		}
	}

	broadcast.mu.Lock()
	attached := false
	for _, s := range broadcast.sinks {
		if s == slow {
			attached = true
		}
	}
	broadcast.mu.Unlock()
	if !attached {
		t.Fatal("slow sink was detached; want it kept and resynced at the next IDR")
	}

	got, err := slow.ReadVideoAccessUnit()
	if err != nil {
		t.Fatalf("slow sink read: %v", err)
	}
	if !bytes.Equal(got.AnnexB, idr2) {
		t.Fatalf("slow sink first AU after stall = %x, want the second IDR %x", got.AnnexB, idr2)
	}
	got, err = slow.ReadVideoAccessUnit()
	if err != nil || !bytes.Equal(got.AnnexB, pFrame(5)) {
		t.Fatalf("slow sink second AU after resync = %x, %v; want %x", got.AnnexB, err, pFrame(5))
	}
	// IDR and P1 fill the budget, so P2 overflows: IDR and P1 are discarded, P2
	// is dropped, and P3 and P4 are dropped while waiting for the IDR.
	if n := slow.DroppedFrames(); n != 5 {
		t.Fatalf("DroppedFrames = %d, want 5 (2 queued + overflowing P + 2 while waiting)", n)
	}

	close(frames)
	select {
	case err := <-runDone:
		if !errors.Is(err, io.EOF) {
			t.Fatalf("broadcast run = %v, want EOF", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("broadcast did not finish")
	}
}

// An overflowing IDR is itself a resync point: the backlog is discarded and the
// IDR is queued at once.
func TestBroadcastDropToKeyframeOverflowingIDRIsQueued(t *testing.T) {
	s := newBroadcastSink(nil)
	s.dropToKeyframe = true
	for _, data := range [][]byte{testIDR, testP} {
		if err := s.enqueueFrame(au(data)); err != nil {
			t.Fatal(err)
		}
	}
	idr2 := append(append([]byte(nil), testIDR...), 0x02)
	if err := s.enqueueFrame(au(idr2)); err != nil {
		t.Fatalf("overflowing IDR: %v", err)
	}
	got, err := s.ReadVideoAccessUnit()
	if err != nil || !bytes.Equal(got.AnnexB, idr2) {
		t.Fatalf("first AU = %x, %v; want the overflowing IDR", got.AnnexB, err)
	}
	if n := s.DroppedFrames(); n != 2 {
		t.Fatalf("DroppedFrames = %d, want 2", n)
	}
}

// Without the opt-in, overflow still detaches the sink (upstream behavior).
func TestBroadcastSinkOverflowDefaultStillErrors(t *testing.T) {
	s := newBroadcastSink(nil)
	for _, data := range [][]byte{testIDR, testP} {
		if err := s.enqueueFrame(au(data)); err != nil {
			t.Fatal(err)
		}
	}
	if err := s.enqueueFrame(au(testP)); !errors.Is(err, errBroadcastSinkBacklog) {
		t.Fatalf("default overflow = %v, want errBroadcastSinkBacklog", err)
	}
}
