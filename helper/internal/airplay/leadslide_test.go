package airplay

import (
	"sync"
	"testing"
	"time"
)

func TestLeadSlideMovesAtTheSlideRate(t *testing.T) {
	t0 := time.Unix(1_000_000, 0)
	l := newLeadSlide(100*time.Millisecond, 160*time.Millisecond, t0)
	if got := l.Effective(t0); got != 100*time.Millisecond {
		t.Fatalf("start effective %v", got)
	}
	l.SetTarget(70*time.Millisecond, t0)
	// 300 ppm: 0.3 ms per second.
	if got := l.Effective(t0.Add(10 * time.Second)); got != 97*time.Millisecond {
		t.Fatalf("after 10 s %v, want 97ms", got)
	}
	if got := l.Effective(t0.Add(200 * time.Second)); got != 70*time.Millisecond {
		t.Fatalf("after 200 s %v, want the 70ms target", got)
	}
	l.SetTarget(150*time.Millisecond, t0.Add(200*time.Second))
	if got := l.Effective(t0.Add(210 * time.Second)); got != 73*time.Millisecond {
		t.Fatalf("raising 10 s %v, want 73ms", got)
	}
}

func TestLeadSlideClampsTargetToFloorAndCeiling(t *testing.T) {
	t0 := time.Unix(1_000_000, 0)
	l := newLeadSlide(100*time.Millisecond, 160*time.Millisecond, t0)
	l.SetTarget(500*time.Millisecond, t0)
	if st := l.State(t0); st.Target != 160*time.Millisecond || st.Ceiling != 160*time.Millisecond {
		t.Fatalf("state %+v, want target clamped to the 160ms ceiling", st)
	}
	l.SetTarget(10*time.Millisecond, t0)
	if st := l.State(t0); st.Target != 40*time.Millisecond {
		t.Fatalf("target %v, want the 40ms floor", st.Target)
	}
	low := newLeadSlide(30*time.Millisecond, 90*time.Millisecond, t0)
	low.SetTarget(10*time.Millisecond, t0)
	if st := low.State(t0); st.Target != 30*time.Millisecond {
		t.Fatalf("target %v, want the 30ms start as floor when start < 40ms", st.Target)
	}
}

func TestLeadSlideTargetChangeMidSlideContinuesFromEffective(t *testing.T) {
	t0 := time.Unix(1_000_000, 0)
	l := newLeadSlide(100*time.Millisecond, 160*time.Millisecond, t0)
	l.SetTarget(40*time.Millisecond, t0)
	t1 := t0.Add(20 * time.Second) // 6 ms down
	l.SetTarget(120*time.Millisecond, t1)
	if got := l.Effective(t1.Add(10 * time.Second)); got != 97*time.Millisecond {
		t.Fatalf("got %v, want 94ms + 3ms = 97ms", got)
	}
}

func TestLeadSlideClockGoingBackwardsDoesNotMove(t *testing.T) {
	t0 := time.Unix(1_000_000, 0)
	l := newLeadSlide(100*time.Millisecond, 160*time.Millisecond, t0)
	l.SetTarget(40*time.Millisecond, t0)
	if got := l.Effective(t0.Add(-time.Second)); got != 100*time.Millisecond {
		t.Fatalf("got %v", got)
	}
}

func TestSetLeadSlidePPMBounds(t *testing.T) {
	defer SetLeadSlidePPM(300)
	for _, bad := range []int{0, -1, 5001} {
		if SetLeadSlidePPM(bad) == nil {
			t.Fatalf("SetLeadSlidePPM(%d) accepted", bad)
		}
	}
	if err := SetLeadSlidePPM(1000); err != nil {
		t.Fatal(err)
	}
	t0 := time.Unix(1_000_000, 0)
	l := newLeadSlide(100*time.Millisecond, 160*time.Millisecond, t0)
	l.SetTarget(40*time.Millisecond, t0)
	if got := l.Effective(t0.Add(10 * time.Second)); got != 90*time.Millisecond {
		t.Fatalf("1000 ppm after 10 s: %v, want 90ms", got)
	}
}

func TestLeadSlideIsSafeForConcurrentUse(t *testing.T) {
	t0 := time.Now()
	l := newLeadSlide(100*time.Millisecond, 160*time.Millisecond, t0)
	var wg sync.WaitGroup
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			for j := 0; j < 1000; j++ {
				if i == 0 {
					l.SetTarget(time.Duration(40+j%100)*time.Millisecond, time.Now())
				} else {
					l.Effective(time.Now())
				}
			}
		}(i)
	}
	wg.Wait()
}
