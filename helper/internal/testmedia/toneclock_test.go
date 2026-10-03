package testmedia

import (
	"testing"
	"time"
)

func TestToneClockDeliversRealTimeDespiteMissedTicks(t *testing.T) {
	start := time.Unix(1_000_000, 0)
	c := NewToneClock(start)
	if got := c.Due(start.Add(5 * time.Millisecond)); len(got) != 0 {
		t.Fatalf("%d blocks before 10 ms, want 0", len(got))
	}
	// One late tick at 47 ms owes four blocks, not one.
	got := c.Due(start.Add(47 * time.Millisecond))
	if len(got) != 4 {
		t.Fatalf("%d blocks at 47 ms, want 4", len(got))
	}
	for i, b := range got {
		if b.Offset != time.Duration(i)*10*time.Millisecond || len(b.PCM) != 441*4 {
			t.Fatalf("block %d: offset %v, %d bytes", i, b.Offset, len(b.PCM))
		}
	}
	if got := c.Due(start.Add(51 * time.Millisecond)); len(got) != 1 || got[0].Offset != 40*time.Millisecond {
		t.Fatalf("at 51 ms got %+v, want the single block at 40 ms", got)
	}
}

func TestToneClockPPMRunsFast(t *testing.T) {
	start := time.Unix(1_000_000, 0)
	c := NewToneClockPPM(start, 1000) // 0.1 % fast
	got := len(c.Due(start.Add(10 * time.Second)))
	if got != 1001 {
		t.Fatalf("%d blocks after 10 s at +1000 ppm, want 1001", got)
	}
}

func TestFrameClockPacesNTSCRateByElapsedTime(t *testing.T) {
	start := time.Unix(1_000_000, 0)
	c := NewFrameClock(start, 60000.0/1001)
	// Frame 0 is due at once; one late check at 50 ms owes frames 0-2 (the
	// fourth is due at 50.05 ms).
	got := c.Due(start.Add(50 * time.Millisecond))
	if len(got) != 3 || got[0] != 0 || got[2] != 2*1001*time.Second/60000 {
		t.Fatalf("at 50 ms got %v, want offsets of frames 0-2", got)
	}
	// 1001 s of 59.94 fps is exactly 60000 frames: one fewer per 16.7 s than 60 fps.
	if n := len(c.Due(start.Add(1001*time.Second))) + 3; n != 60001 {
		t.Fatalf("%d frames by 1001 s, want 60001 (frames 0-60000)", n)
	}
}
