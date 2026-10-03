package airplay

import (
	"fmt"
	"sync"
	"sync/atomic"
	"time"
)

// LeadState is a session's playout lead: the one in use, the one it is
// sliding toward, and the ceiling announced at SETUP.
type LeadState struct {
	Effective time.Duration
	Target    time.Duration
	Ceiling   time.Duration
}

// leadSlideFloor is the lowest target a slide accepts (the plugin's TV delay
// floor). A session that started below it keeps its start as the floor.
const leadSlideFloor = 40 * time.Millisecond

var leadSlidePPM atomic.Int64

// minDuration returns the minimum of two durations.
func minDuration(a, b time.Duration) time.Duration {
	if a < b {
		return a
	}
	return b
}

// maxDuration returns the maximum of two durations.
func maxDuration(a, b time.Duration) time.Duration {
	if a > b {
		return a
	}
	return b
}

func init() { leadSlidePPM.Store(300) }

// SetLeadSlidePPM sets the rate at which new sessions slide their lead, in
// parts per million of elapsed time (300: 0.3 ms per second). For on-site
// tests; the default is the rate the Roku followed cleanly (tone test,
// 2026-10-01).
func SetLeadSlidePPM(ppm int) error {
	if ppm < 1 || ppm > 5000 {
		return fmt.Errorf("lead slide %d ppm: want 1 to 5000", ppm)
	}
	leadSlidePPM.Store(int64(ppm))
	return nil
}

// leadSlide moves a live session's playout lead toward a target by elapsed
// time, at most ppm of the time elapsed, so a receiver sees what looks like
// a sender clock running slightly fast or slow instead of a jump (spec
// 2026-10-02 section 2). Video writer, audio sender and sync ticker read it;
// set_lead writes it.
type leadSlide struct {
	mu        sync.Mutex
	floor     time.Duration
	ceiling   time.Duration
	target    time.Duration
	effective time.Duration
	last      time.Time
	ppm       float64
}

func newLeadSlide(start, ceiling time.Duration, now time.Time) *leadSlide {
	if ceiling < start {
		ceiling = start
	}
	floor := leadSlideFloor
	if start < floor {
		floor = start
	}
	return &leadSlide{floor: floor, ceiling: ceiling, target: start, effective: start, last: now, ppm: float64(leadSlidePPM.Load())}
}

func (l *leadSlide) advanceLocked(now time.Time) {
	if !now.After(l.last) {
		return
	}
	step := time.Duration(float64(now.Sub(l.last)) * l.ppm / 1e6)
	l.last = now
	switch diff := l.target - l.effective; {
	case diff > 0:
		l.effective += minDuration(step, diff)
	case diff < 0:
		l.effective -= minDuration(step, -diff)
	}
}

// Effective returns the lead in use at now.
func (l *leadSlide) Effective(now time.Time) time.Duration {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.advanceLocked(now)
	return l.effective
}

// SetTarget sets the lead to slide toward, clamped to the floor and ceiling.
func (l *leadSlide) SetTarget(d time.Duration, now time.Time) {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.advanceLocked(now)
	l.target = maxDuration(l.floor, minDuration(d, l.ceiling))
}

// State returns effective, target and ceiling at now.
func (l *leadSlide) State(now time.Time) LeadState {
	l.mu.Lock()
	defer l.mu.Unlock()
	l.advanceLocked(now)
	return LeadState{Effective: l.effective, Target: l.target, Ceiling: l.ceiling}
}

// sessionLeadForSetup returns the latencies to announce at SETUP and, for a
// session with a per-session lead and headroom, its slider. The slider
// starts at the session lead; the ceiling is announced.
func sessionLeadForSetup(cfg StreamConfig, latencies screenLatencyTargets, now time.Time) (screenLatencyTargets, *leadSlide) {
	if cfg.TargetLatency <= 0 || cfg.LeadHeadroom <= 0 {
		return latencies, nil
	}
	start := latencies.video
	ceiling := clampLead(start + cfg.LeadHeadroom)
	return screenLatencyTargets{video: ceiling, audio: ceiling}, newLeadSlide(start, ceiling, now)
}

// SetLeadTarget sets a live session's lead target (set_lead). A session
// without headroom has a fixed lead and refuses.
func (s *MirrorSession) SetLeadTarget(d time.Duration) (LeadState, error) {
	if s.lead == nil {
		return LeadState{}, fmt.Errorf("this session's lead is fixed")
	}
	now := time.Now()
	s.lead.SetTarget(d, now)
	return s.lead.State(now), nil
}

// LeadState reports the session's lead now; a fixed session reports its
// SETUP lead for all three values.
func (s *MirrorSession) LeadState() LeadState {
	if s.lead == nil {
		return LeadState{Effective: s.timestampBias, Target: s.timestampBias, Ceiling: s.timestampBias}
	}
	return s.lead.State(time.Now())
}

// audioSyncOffset is how far before the capture instant a sync packet's
// network time is placed, so a receiver that plays at network time plus the
// announced ceiling plays at capture plus the effective lead.
func (s *MirrorSession) audioSyncOffset(now time.Time) time.Duration {
	if s.lead == nil {
		return 0
	}
	st := s.lead.State(now)
	return st.Effective - st.Ceiling
}

// audioLeadAt is the lead the stale-audio check measures against.
func (s *MirrorSession) audioLeadAt(now time.Time, as *AudioStream) time.Duration {
	if s.lead != nil {
		return s.lead.Effective(now)
	}
	return audioLatencyDuration(as.latencySamples)
}
