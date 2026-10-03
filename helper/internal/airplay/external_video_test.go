package airplay

import (
	"bytes"
	"errors"
	"io"
	"testing"
	"time"
)

var (
	testIDR = []byte{0, 0, 0, 1, 0x67, 0x42, 0, 0x1f, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80, 0, 0, 0, 1, 0x65, 0x88, 0x84}
	testP   = []byte{0, 0, 0, 1, 0x41, 0x9a, 0x02}
)

func au(data []byte) VideoAccessUnit { return VideoAccessUnit{AnnexB: data, PTS: time.Now()} }

func TestExternalAUHasIDR(t *testing.T) {
	if !externalAUHasIDR(testIDR) {
		t.Fatal("IDR not detected")
	}
	if externalAUHasIDR(testP) {
		t.Fatal("P frame reported as IDR")
	}
}

func TestExternalVideoSourceDeliversInOrder(t *testing.T) {
	s := NewExternalVideoSource(4)
	for _, d := range [][]byte{testIDR, testP} {
		if err := s.Push(au(d)); err != nil {
			t.Fatal(err)
		}
	}
	for i, want := range [][]byte{testIDR, testP} {
		got, err := s.ReadVideoAccessUnit()
		if err != nil || !bytes.Equal(got.AnnexB, want) {
			t.Fatalf("read %d = %x, %v", i, got.AnnexB, err)
		}
	}
}

func TestExternalVideoSourceDropsUntilIDRAfterOverflow(t *testing.T) {
	s := NewExternalVideoSource(2)
	s.Push(au(testIDR))
	s.Push(au(testP))
	s.Push(au(testP)) // overflow: dropped, now waiting for IDR
	s.ReadVideoAccessUnit()
	s.ReadVideoAccessUnit()
	s.Push(au(testP)) // dropped: still waiting for IDR
	idr2 := append([]byte(nil), testIDR...)
	idr2 = append(idr2, 0x01)
	s.Push(au(idr2))
	s.Push(au(testP))
	got, _ := s.ReadVideoAccessUnit()
	if !bytes.Equal(got.AnnexB, idr2) {
		t.Fatalf("after overflow first frame = %x, want the second IDR", got.AnnexB)
	}
	if s.Dropped() != 2 {
		t.Fatalf("Dropped = %d, want 2", s.Dropped())
	}
}

func TestExternalVideoSourceClose(t *testing.T) {
	s := NewExternalVideoSource(2)
	s.Close()
	if _, err := s.ReadVideoAccessUnit(); err != io.EOF {
		t.Fatalf("read after close: %v, want io.EOF", err)
	}
	if err := s.Push(au(testIDR)); !errors.Is(err, ErrExternalSourceClosed) {
		t.Fatalf("push after close: %v", err)
	}
	s.Close() // idempotent
}

func TestExternalVideoCaptureStopReturns(t *testing.T) {
	s := NewExternalVideoSource(2)
	c := s.Capture()
	done := make(chan struct{})
	go func() { c.Stop(); close(done) }()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("ScreenCapture.Stop did not return")
	}
}

func TestExternalVideoFeedsBroadcastSinks(t *testing.T) {
	s := NewExternalVideoSource(8)
	bc := NewBroadcastCaptureWithFrameRate(s.Capture(), 30)
	a, b := bc.AddSink(), bc.AddSink()
	runDone := make(chan error, 1)
	go func() { runDone <- bc.Run() }()
	s.Push(au(testIDR))
	for name, sink := range map[string]*BroadcastSink{"a": a, "b": b} {
		got, err := sink.ReadVideoAccessUnit()
		if err != nil || !bytes.Equal(got.AnnexB, testIDR) {
			t.Fatalf("sink %s = %x, %v", name, got.AnnexB, err)
		}
	}
	s.Close()
	select {
	case <-runDone:
	case <-time.After(5 * time.Second):
		t.Fatal("BroadcastCapture.Run did not return after source close")
	}
}
