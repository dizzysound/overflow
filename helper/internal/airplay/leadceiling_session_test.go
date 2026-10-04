package airplay

import (
	"testing"
	"time"
)

// Narthex on 2026-10-04: an Auto Wi-Fi session that started at 197 ms with
// 100 ms of headroom (297 ms announced at SETUP). At 10:57:02 OBS added 362 ms
// of audio buffering, so the audio reaching the helper went from about 30 ms
// old to about 389 ms old and stayed there.
const (
	ceilingTestStart    = 197 * time.Millisecond
	ceilingTestHeadroom = 100 * time.Millisecond
	ageBeforeJump       = 30 * time.Millisecond
	ageAfterJump        = 389 * time.Millisecond
)

func ceilingTestSession(start time.Duration, t0 time.Time) (*MirrorSession, screenLatencyTargets) {
	lat, slide := sessionLeadForSetup(StreamConfig{TargetLatency: start, LeadHeadroom: ceilingTestHeadroom},
		screenLatencyTargets{video: start, audio: start}, t0)
	return &MirrorSession{timestampBias: lat.video, lead: slide}, lat
}

func staleAt(s *MirrorSession, now time.Time, age time.Duration) bool {
	return audioFrameIsStale(now.Add(-age), now, s.audioLeadAt(now, &AudioStream{}))
}

// The 2026-10-04 condition: after the jump every audio frame is late, and no
// set_lead can fix it, because the slide stops at the ceiling announced at
// SETUP. Only a new session announces more.
func TestAudioAgeJumpAboveTheCeilingIsLateUntilReconnect(t *testing.T) {
	t0 := time.Now()
	s, lat := ceilingTestSession(ceilingTestStart, t0)
	if lat.audio != 297*time.Millisecond {
		t.Fatalf("announced %v, want 297ms", lat.audio)
	}
	if staleAt(s, t0, ageBeforeJump) {
		t.Fatal("30 ms old audio is late at a 197 ms lead")
	}
	jump := t0.Add(2 * time.Minute)
	if !staleAt(s, jump, ageAfterJump) {
		t.Fatal("389 ms old audio is not late at a 197 ms lead")
	}
	// The plugin asks for far more than the ceiling; the slide runs for an hour.
	st, err := s.SetLeadTarget(2 * time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if st.Target != 297*time.Millisecond || st.Ceiling != 297*time.Millisecond {
		t.Fatalf("set_lead 2000 ms gave %+v, want target clamped to the 297 ms ceiling", st)
	}
	hourLater := jump.Add(time.Hour)
	if got := s.lead.Effective(hourLater); got != 297*time.Millisecond {
		t.Fatalf("effective %v after an hour, want 297ms", got)
	}
	if !staleAt(s, hourLater, ageAfterJump) {
		t.Fatal("389 ms old audio is not late at the 297 ms ceiling")
	}

	// The plugin's ceiling guard reconnects at 520 ms (389 + 11 + 50 + 70):
	// the new session announces 620 ms and the same audio is on time.
	reconnect := hourLater.Add(time.Second)
	s2, lat2 := ceilingTestSession(520*time.Millisecond, reconnect)
	if lat2.audio != 620*time.Millisecond {
		t.Fatalf("new session announced %v, want 620ms", lat2.audio)
	}
	if staleAt(s2, reconnect, ageAfterJump) {
		t.Fatal("389 ms old audio is still late after reconnecting at 520 ms")
	}
	// Auto dynamic may lower it again, never below what the audio needs.
	if _, err := s2.SetLeadTarget(400 * time.Millisecond); err != nil {
		t.Fatal(err)
	}
	if staleAt(s2, reconnect.Add(10*time.Minute), ageAfterJump) {
		t.Fatal("389 ms old audio is late at a 400 ms lead")
	}
}

// A reconnect lead near the limit: the announced ceiling stays at 2 s.
func TestReconnectLeadNearTheLimitAnnouncesAtMostTwoSeconds(t *testing.T) {
	_, lat := ceilingTestSession(1950*time.Millisecond, time.Now())
	if lat.audio != 2*time.Second {
		t.Fatalf("announced %v, want 2s", lat.audio)
	}
}
