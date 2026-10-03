package airplay

import (
	"sync/atomic"
	"testing"
	"time"
)

func TestSinkResyncCallbackFiresOncePerEpisode(t *testing.T) {
	frames := make(chan VideoAccessUnit)
	capture := &ScreenCapture{frames: &channelVideoAccessUnitReader{frames: frames}, waitCh: make(chan struct{})}
	b := NewBroadcastCaptureWithFrameRate(capture, 60)
	b.SetDropToKeyframe(true)
	sink := b.AddSink()
	var calls atomic.Int32
	sink.SetOnResync(func() { calls.Add(1) })
	go func() { _ = b.Run() }()
	base := time.Now()
	for i := 0; i < 12; i++ { // unread sink: overflows the 67 ms budget, then keeps dropping
		data := append(append([]byte(nil), testP...), byte(i))
		if i == 0 {
			data = testIDR
		}
		frames <- VideoAccessUnit{AnnexB: data, PTS: base.Add(time.Duration(i) * time.Second / 60)}
	}
	close(frames)
	deadline := time.Now().Add(2 * time.Second)
	for calls.Load() == 0 && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	if got := calls.Load(); got != 1 {
		t.Fatalf("resync callback ran %d times, want exactly 1 for one shed episode", got)
	}
}

func TestSinkReportsHowLongItHasWaitedForIDR(t *testing.T) {
	frames := make(chan VideoAccessUnit)
	capture := &ScreenCapture{frames: &channelVideoAccessUnitReader{frames: frames}, waitCh: make(chan struct{})}
	b := NewBroadcastCaptureWithFrameRate(capture, 60)
	b.SetDropToKeyframe(true)
	sink := b.AddSink()
	go func() { _ = b.Run() }()
	defer close(frames)
	if _, waiting := sink.WaitingForIDRSince(); waiting {
		t.Fatal("a fresh sink reports waiting for an IDR")
	}
	before := time.Now()
	base := before
	for i := 0; i < 12; i++ { // unread sink: overflows the budget, then waits for an IDR
		data := append(append([]byte(nil), testP...), byte(i))
		if i == 0 {
			data = testIDR
		}
		frames <- VideoAccessUnit{AnnexB: data, PTS: base.Add(time.Duration(i) * time.Second / 60)}
	}
	// One more send: the unbuffered channel returns only after the previous frame was fanned out.
	frames <- VideoAccessUnit{AnnexB: append(append([]byte(nil), testP...), 0x40), PTS: base.Add(13 * time.Second / 60)}
	since, waiting := sink.WaitingForIDRSince()
	if !waiting {
		t.Fatal("an overflowed sink does not report waiting for an IDR")
	}
	if since.Before(before) || since.After(time.Now()) {
		t.Fatalf("waiting since %v, want between %v and now", since, before)
	}
	frames <- VideoAccessUnit{AnnexB: testIDR, PTS: base.Add(14 * time.Second / 60)}
	frames <- VideoAccessUnit{AnnexB: append(append([]byte(nil), testP...), 0x41), PTS: base.Add(15 * time.Second / 60)}
	if _, waiting := sink.WaitingForIDRSince(); waiting {
		t.Fatal("the sink still reports waiting after an IDR arrived")
	}
}

func TestBroadcastIDRObserverReportsIDRsOnlyWhileWanted(t *testing.T) {
	frames := make(chan VideoAccessUnit)
	capture := &ScreenCapture{frames: &channelVideoAccessUnitReader{frames: frames}, waitCh: make(chan struct{})}
	b := NewBroadcastCaptureWithFrameRate(capture, 60)
	var want atomic.Bool
	var saw atomic.Int32
	b.SetIDRObserver(want.Load, func() { saw.Add(1) })
	go func() { _ = b.Run() }()
	defer close(frames)
	base := time.Now()
	n := 0
	send := func(data []byte) {
		frames <- VideoAccessUnit{AnnexB: data, PTS: base.Add(time.Duration(n) * time.Second / 60)}
		n++
	}
	send(testIDR) // not wanted
	send(testP)   // flushes the IDR before want changes
	want.Store(true)
	send(testP) // wanted, not an IDR
	send(testP) // flushes the previous frame
	if got := saw.Load(); got != 0 {
		t.Fatalf("observer saw %d IDRs before a wanted IDR, want 0", got)
	}
	send(testIDR)
	send(testP) // flushes the IDR
	if got := saw.Load(); got != 1 {
		t.Fatalf("observer saw %d IDRs, want 1", got)
	}
}
