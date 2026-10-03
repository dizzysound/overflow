package bridge

import (
	"math/rand"
	"testing"
	"time"
)

// simClock drives a clockMap the way the church PC does: OBS stamps with a
// precise clock (QPC), the helper reads Go's time.Now, which on Windows moves
// only at the system timer tick, and each message spends a random time in the pipe.
type simClock struct {
	t      *testing.T
	cm     *clockMap
	rng    *rand.Rand
	epoch  time.Time     // helper time at plugin ns 0, the truth
	tick   time.Duration // helper clock granularity
	maxLag time.Duration // pipe transit, uniform 0..maxLag
}

func (s *simClock) helperNow(trueNs int64) time.Time {
	// Truncate to the tick, as INTERRUPT_TIME does.
	q := trueNs - trueNs%int64(s.tick)
	return s.epoch.Add(time.Duration(q))
}

// send delivers one message whose sample was captured age before it was sent.
func (s *simClock) send(sendNs uint64, age time.Duration) time.Time {
	arrive := int64(sendNs) + s.rng.Int63n(int64(s.maxLag)+1)
	s.cm.Observe(s.helperNow(arrive), sendNs)
	return s.cm.Map(sendNs - uint64(age))
}

func TestClockMapRemovesTickAndTransitJitter(t *testing.T) {
	s := &simClock{t: t, cm: newClockMap(clockMapWindow, clockMapSlewPPM, clockMapStep), rng: rand.New(rand.NewSource(7)),
		epoch: time.Unix(5000, 0), tick: time.Millisecond, maxLag: 300 * time.Microsecond}
	const start = uint64(3_600_000_000_000) // plugin clock about 1 h after boot
	var prevMapped time.Time
	var prevCapture uint64
	for i := 0; i < 1000; i++ { // 10 s of messages every 10 ms
		send := start + uint64(i)*10_000_000 + uint64(s.rng.Int63n(2_000_000))
		age := time.Duration(20+s.rng.Int63n(5)) * time.Millisecond
		mapped := s.send(send, age)
		capture := send - uint64(age)
		if i > 300 { // after the first window has settled
			want := time.Duration(capture - prevCapture)
			if got := mapped.Sub(prevMapped); got-want > 10*time.Microsecond || want-got > 10*time.Microsecond {
				t.Fatalf("message %d: mapped step %v, capture step %v", i, got, want)
			}
		}
		// Absolute error stays within one tick plus the transit bound.
		truth := s.epoch.Add(time.Duration(capture))
		if d := mapped.Sub(truth); d > s.tick || -d > s.tick+s.maxLag {
			t.Fatalf("message %d: mapped %v from the truth", i, d)
		}
		prevMapped, prevCapture = mapped, capture
	}
}

func TestClockMapStepsOnLargeJump(t *testing.T) {
	cm := newClockMap(clockMapWindow, clockMapSlewPPM, clockMapStep)
	base := time.Unix(100, 0)
	for i := uint64(0); i < 400; i++ { // 4 s settles past the start window
		cm.Observe(base.Add(time.Duration(i*10_000_000)), i*10_000_000)
	}
	// The helper clock jumps 1 s ahead of the plugin clock (for example after a resume).
	jumped := base.Add(time.Second)
	for i := uint64(400); i < 430; i++ { // 300 ms of messages, past clockMapRaise
		cm.Observe(jumped.Add(time.Duration(i*10_000_000)), i*10_000_000)
	}
	got := cm.Map(429 * 10_000_000)
	want := jumped.Add(429 * 10_000_000)
	if d := got.Sub(want); d > time.Millisecond || d < -time.Millisecond {
		t.Fatalf("after a 1 s forward jump the mapping is %v off", d)
	}
	// A backward jump is followed at the next message.
	back := base.Add(-time.Second)
	cm.Observe(back.Add(430*10_000_000), 430*10_000_000)
	if d := cm.Map(430 * 10_000_000).Sub(back.Add(430 * 10_000_000)); d > time.Millisecond || d < -time.Millisecond {
		t.Fatalf("after a 1 s backward jump the mapping is %v off", d)
	}
}

func TestClockMapIgnoresOneSlowRead(t *testing.T) {
	cm := newClockMap(clockMapWindow, clockMapSlewPPM, clockMapStep)
	base := time.Unix(100, 0)
	for i := uint64(0); i < 400; i++ {
		cm.Observe(base.Add(time.Duration(i*10_000_000)), i*10_000_000)
	}
	before := cm.Map(0)
	// The helper stalls 100 ms: ten messages arrive late in a burst.
	for i := uint64(400); i < 410; i++ {
		cm.Observe(base.Add(4100*time.Millisecond), i*10_000_000)
	}
	if shift := cm.Map(0).Sub(before); shift > 10*time.Microsecond || shift < -10*time.Microsecond {
		t.Fatalf("one slow read moved the mapping %v", shift)
	}
}

func TestClockMapSlewsSmallChanges(t *testing.T) {
	cm := newClockMap(clockMapWindow, clockMapSlewPPM, clockMapStep)
	base := time.Unix(100, 0)
	for i := uint64(0); i < 400; i++ {
		cm.Observe(base.Add(time.Duration(i*10_000_000)), i*10_000_000)
	}
	before := cm.Map(0)
	// The offset moves 2 ms (below the step threshold). After the window has
	// expired the old minimum, 1 s of messages may move the mapping by at most
	// 200 ppm of 1 s = 200 us beyond what the window held.
	moved := base.Add(2 * time.Millisecond)
	for i := uint64(400); i < 700; i++ {
		cm.Observe(moved.Add(time.Duration(i*10_000_000)), i*10_000_000)
	}
	shift := cm.Map(0).Sub(before)
	if shift <= 0 || shift > 300*time.Microsecond {
		t.Fatalf("mapping moved %v in 1 s after the window, want 0 < shift <= 300us (200 ppm slew)", shift)
	}
}

func TestClockMapBeforeObserveIsZero(t *testing.T) {
	if got := newClockMap(clockMapWindow, clockMapSlewPPM, clockMapStep).Map(5); !got.IsZero() {
		t.Fatalf("Map before Observe = %v, want zero time", got)
	}
}
