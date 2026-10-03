package airplay

import (
	"encoding/binary"
	"testing"
	"time"
)

func slidingSession(t0 time.Time) *MirrorSession {
	return &MirrorSession{
		timestampBias: 160 * time.Millisecond, // the announced ceiling
		lead:          newLeadSlide(100*time.Millisecond, 160*time.Millisecond, t0),
	}
}

func TestVideoTimestampsFollowTheEffectiveLead(t *testing.T) {
	t0 := time.Now()
	s := slidingSession(t0)
	fixed := &MirrorSession{timestampBias: 100 * time.Millisecond}
	a, _, _ := s.frameTimeAtNow(t0, t0)
	b, _, _ := fixed.frameTimeAtNow(t0, t0)
	if d := int64(a) - int64(b); d < -int64(compactTimestamp(time.Millisecond)) || d > int64(compactTimestamp(time.Millisecond)) {
		t.Fatalf("sliding session stamps %v away from a fixed 100ms lead, want the effective 100ms", time.Duration(d))
	}
	// A fresh session: the first one's monotonic frame clamp would hold the
	// stamp at its earlier, higher value because the test clock never advances.
	s = slidingSession(t0)
	s.lead.SetTarget(40*time.Millisecond, t0)
	later := t0.Add(100 * time.Second) // 30 ms lower
	c, _, _ := s.frameTimeAtNow(later, later)
	fixed70 := &MirrorSession{timestampBias: 70 * time.Millisecond}
	e, _, _ := fixed70.frameTimeAtNow(later, later)
	if d := int64(c) - int64(e); d < -int64(compactTimestamp(time.Millisecond)) || d > int64(compactTimestamp(time.Millisecond)) {
		t.Fatalf("after 100 s at 300 ppm the stamp is %v off a 70ms lead", time.Duration(d))
	}
}

func TestAudioSyncMappingCarriesTheLeadOffset(t *testing.T) {
	t0 := time.Now()
	s := slidingSession(t0)
	plain := &MirrorSession{}
	pts := t0.Add(time.Second) // ahead: boot-relative time can be tiny early in a test process
	got, _ := s.audioClockAt(pts)
	want, _ := plain.audioClockAt(pts)
	// Effective 100 ms under a 160 ms ceiling: the pairing is 60 ms earlier,
	// so the receiver plays at capture + 160 - 60 = capture + 100 ms.
	diff := time.Duration(int64(want-got)) * time.Second >> 32
	if diff < 59*time.Millisecond || diff > 61*time.Millisecond {
		t.Fatalf("sync network time is %v earlier, want 60ms", diff)
	}
}

func TestSyncPacketLatencyFieldStaysAtTheCeiling(t *testing.T) {
	stream := &AudioStream{latencySamples: samplesFor44k1(160 * time.Millisecond)}
	conn := &recordingPacketConn{}
	stream.ctrlConn = conn
	stream.ctrlAddr = &netUDPAddrForAudioTest
	if err := stream.sendSyncPacketAt(timingProtocolNTP, 0x83aa7e8012345678, 0, 100000, false); err != nil {
		t.Fatal(err)
	}
	if got := 100000 - binary.BigEndian.Uint32(conn.packets[0][4:8]); got != samplesFor44k1(160*time.Millisecond) {
		t.Fatalf("latency field %d samples, want the 160ms ceiling", got)
	}
}

func TestStaleAudioUsesTheEffectiveLead(t *testing.T) {
	t0 := time.Now()
	s := slidingSession(t0)
	s.lead.SetTarget(40*time.Millisecond, t0)
	later := t0.Add(100 * time.Second) // effective 70 ms
	if got := s.audioLeadAt(later, &AudioStream{latencySamples: samplesFor44k1(160 * time.Millisecond)}); got != 70*time.Millisecond {
		t.Fatalf("audio lead %v, want 70ms effective", got)
	}
	fixed := &MirrorSession{}
	if got := fixed.audioLeadAt(later, &AudioStream{latencySamples: 4410}); got != 100*time.Millisecond {
		t.Fatalf("fixed audio lead %v, want 100ms from latencySamples", got)
	}
}

func TestSetLeadTargetOnAFixedSessionFails(t *testing.T) {
	if _, err := (&MirrorSession{timestampBias: 100 * time.Millisecond}).SetLeadTarget(70 * time.Millisecond); err == nil {
		t.Fatal("fixed session accepted a lead target")
	}
	s := slidingSession(time.Now())
	st, err := s.SetLeadTarget(70 * time.Millisecond)
	if err != nil || st.Target != 70*time.Millisecond || st.Ceiling != 160*time.Millisecond {
		t.Fatalf("state %+v err %v", st, err)
	}
}

func TestLeadStateOfAFixedSessionIsItsBias(t *testing.T) {
	st := (&MirrorSession{timestampBias: 100 * time.Millisecond}).LeadState()
	if st.Effective != 100*time.Millisecond || st.Target != st.Effective || st.Ceiling != st.Effective {
		t.Fatalf("state %+v", st)
	}
}

func TestSessionLeadForSetup(t *testing.T) {
	t0 := time.Now()
	lat, slide := sessionLeadForSetup(StreamConfig{TargetLatency: 70 * time.Millisecond, LeadHeadroom: 60 * time.Millisecond}, screenLatencyTargets{video: 70 * time.Millisecond, audio: 70 * time.Millisecond}, t0)
	if lat.video != 130*time.Millisecond || lat.audio != 130*time.Millisecond || slide == nil || slide.Effective(t0) != 70*time.Millisecond {
		t.Fatalf("announced %+v slide %v, want 130ms announced and 70ms effective", lat, slide)
	}
	lat, slide = sessionLeadForSetup(StreamConfig{TargetLatency: 70 * time.Millisecond}, screenLatencyTargets{video: 70 * time.Millisecond, audio: 70 * time.Millisecond}, t0)
	if lat.video != 70*time.Millisecond || slide != nil {
		t.Fatalf("no headroom: announced %+v slide %v, want today's 70ms and no slide", lat, slide)
	}
	lat, slide = sessionLeadForSetup(StreamConfig{LeadHeadroom: 60 * time.Millisecond}, screenLatencyTargets{video: 75 * time.Millisecond, audio: 85 * time.Millisecond}, t0)
	if lat.video != 75*time.Millisecond || slide != nil {
		t.Fatalf("headroom without a per-session lead must not slide: %+v %v", lat, slide)
	}
}

func TestUntimestampedAudioSyncCarriesTheLeadOffset(t *testing.T) {
	t0 := time.Now()
	s := slidingSession(t0) // effective 100 ms under a 160 ms ceiling
	plain := &MirrorSession{}
	got, _ := s.audioClockNow()
	want, _ := plain.audioClockNow()
	diff := time.Duration(int64(want-got)) * time.Second >> 32
	if diff < 58*time.Millisecond || diff > 62*time.Millisecond {
		t.Fatalf("untimestamped sync network time is %v earlier, want 60ms", diff)
	}
}
