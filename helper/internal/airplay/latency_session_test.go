package airplay

import (
	"testing"
	"time"
)

func TestSessionLatenciesUsesPerSessionLead(t *testing.T) {
	SetTargetLatency(200 * time.Millisecond)
	t.Cleanup(func() { SetTargetLatency(0) })
	got := sessionLatencies(StreamConfig{TargetLatency: 95 * time.Millisecond})
	if got.video != 95*time.Millisecond || got.audio != 95*time.Millisecond {
		t.Fatalf("sessionLatencies = %+v, want video=audio=95ms over the 200ms global", got)
	}
	if !latencyIsExplicit(StreamConfig{TargetLatency: 95 * time.Millisecond}) {
		t.Fatal("latencyIsExplicit = false for a per-session lead")
	}
}

func TestSessionLatenciesFallsBackToGlobalPolicy(t *testing.T) {
	SetTargetLatency(0)
	got := sessionLatencies(StreamConfig{})
	if got.video != defaultVideoLatencyNormal || got.audio != defaultAudioLatencyNormal {
		t.Fatalf("sessionLatencies = %+v, want Apple Normal defaults", got)
	}
	if latencyIsExplicit(StreamConfig{}) {
		t.Fatal("latencyIsExplicit = true with no global or session lead")
	}
}

func TestSessionLatenciesClampsPerSessionLead(t *testing.T) {
	if got := sessionLatencies(StreamConfig{TargetLatency: time.Millisecond}); got.video != 5*time.Millisecond {
		t.Fatalf("low clamp: %v", got.video)
	}
	if got := sessionLatencies(StreamConfig{TargetLatency: 5 * time.Second}); got.video != 2*time.Second {
		t.Fatalf("high clamp: %v", got.video)
	}
}
