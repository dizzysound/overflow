package bridge

import (
	"testing"
	"time"

	"doubletake/internal/airplay"
)

func TestDeliveryWindowPercentilesAndLate(t *testing.T) {
	w := newDeliveryWindows()
	for i := 1; i <= 100; i++ { // ages 1..100 ms, lead 90 ms: 10 late
		w.add("A", airplay.LeadState{Effective: 90 * time.Millisecond, Target: 90 * time.Millisecond, Ceiling: 90 * time.Millisecond}, time.Duration(i)*time.Millisecond)
	}
	got := w.flush()
	if len(got) != 1 {
		t.Fatalf("flush = %+v, want one display", got)
	}
	s := got[0]
	if s.DeviceID != "A" || s.Frames != 100 || s.P50Ms != 50 || s.P99Ms != 99 || s.Late != 10 || s.LeadMs != 90 {
		t.Fatalf("summary = %+v", s)
	}
	if again := w.flush(); len(again) != 0 {
		t.Fatalf("second flush = %+v, want empty (windows reset)", again)
	}
}

func TestDeliveryWindowCountsAudioResends(t *testing.T) {
	w := newDeliveryWindows()
	w.add("A", airplay.LeadState{Effective: 200 * time.Millisecond, Target: 200 * time.Millisecond, Ceiling: 200 * time.Millisecond}, 50*time.Millisecond)
	w.addResend("A", 5, 5)
	w.addResend("A", 2, 1)
	got := w.flush()
	if len(got) != 1 || got[0].AudioLost != 7 || got[0].AudioResent != 6 {
		t.Fatalf("flush = %+v, want A with 7 lost, 6 resent", got)
	}
	w.add("A", airplay.LeadState{Effective: 200 * time.Millisecond, Target: 200 * time.Millisecond, Ceiling: 200 * time.Millisecond}, 50*time.Millisecond)
	if again := w.flush(); len(again) != 1 || again[0].AudioLost != 0 {
		t.Fatalf("second window = %+v, want resend counts reset", again)
	}
}

func TestDeliveryWindowCountsDroppedAudio(t *testing.T) {
	w := newDeliveryWindows()
	w.add("A", airplay.LeadState{Effective: 70 * time.Millisecond, Target: 70 * time.Millisecond, Ceiling: 70 * time.Millisecond}, 30*time.Millisecond)
	w.addDropped("A")
	w.addDropped("A")
	if got := w.flush(); len(got) != 1 || got[0].AudioDropped != 2 {
		t.Fatalf("flush = %+v, want A with 2 dropped", got)
	}
	w.add("A", airplay.LeadState{Effective: 70 * time.Millisecond, Target: 70 * time.Millisecond, Ceiling: 70 * time.Millisecond}, 30*time.Millisecond)
	if again := w.flush(); again[0].AudioDropped != 0 {
		t.Fatalf("second window = %+v, want dropped reset", again)
	}
}

func TestDeliveryReportsEffectiveTargetAndCeiling(t *testing.T) {
	w := newDeliveryWindows()
	w.add("AA", airplay.LeadState{Effective: 80 * time.Millisecond, Target: 70 * time.Millisecond, Ceiling: 130 * time.Millisecond}, 20*time.Millisecond)
	w.add("AA", airplay.LeadState{Effective: 79 * time.Millisecond, Target: 70 * time.Millisecond, Ceiling: 130 * time.Millisecond}, 90*time.Millisecond)
	s := w.flush()
	if len(s) != 1 || s[0].LeadMs != 79 || s[0].LeadTargetMs != 70 || s[0].LeadCeilingMs != 130 || s[0].Late != 1 {
		t.Fatalf("summary %+v, want lead 79 (last), target 70, ceiling 130, 1 late", s)
	}
}

func TestDeliveryCountsAudioJumps(t *testing.T) {
	w := newDeliveryWindows()
	w.add("AA", airplay.LeadState{}, 10*time.Millisecond)
	w.addJump("AA")
	w.addJump("AA")
	got := w.flush()
	if len(got) != 1 || got[0].AudioJumps != 2 {
		t.Fatalf("flush = %+v, want AudioJumps 2", got)
	}
	if again := w.flush(); len(again) != 0 {
		t.Fatalf("second flush = %+v, want empty", again)
	}
}
