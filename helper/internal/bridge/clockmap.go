package bridge

import (
	"sync"
	"time"
)

const (
	// clockMapWindow is how long a raw offset estimate competes for the minimum.
	clockMapWindow = 2 * time.Second
	// clockMapSlewPPM bounds how fast the mapping follows a changed minimum:
	// 200 ppm is 0.2 ms per second, slow enough that receivers see a smooth
	// timeline, fast enough to track any real drift between the two clocks.
	clockMapSlewPPM = 200
	// clockMapStep is the change that is applied at once instead of slewed.
	clockMapStep = 20 * time.Millisecond
	// clockMapRaise is how long every estimate must sit above the mapping by
	// more than step before a forward jump is accepted. The window minimum
	// follows a backward jump at once but would hold a forward one for a whole
	// window; one slow read of the pipe must not count as a jump.
	clockMapRaise = 250 * time.Millisecond
)

// clockMap maps plugin timestamps (OBS os_gettime_ns) onto the helper clock.
//
// Each message gives one raw estimate: the helper's receive time minus the
// plugin's send stamp. On Windows Go's time.Now advances only at the system
// timer tick (the runtime reads INTERRUPT_TIME) while OBS stamps with
// QueryPerformanceCounter, and the message spends a variable time in the pipe,
// so raw estimates scatter by up to a tick plus transit. Mapping each message
// with its own estimate (the protocol's original helper_now - age) put that
// scatter into every timestamp; for audio it became RTP gaps
// (data/2026-09-28-quality-latency/16-rtp-gaps.md).
//
// clockMap keeps the minimum raw estimate over a sliding window, as NTP keeps
// its lowest-delay sample, and slews toward it, so mapped times move only with
// the true relation between the clocks. A change larger than step (a clock
// jump) is applied at once.
type clockMap struct {
	mu      sync.Mutex
	window  time.Duration
	slewPPM int64
	step    time.Duration

	have    bool
	ref     time.Time     // first receive time; estimates are offsets from it
	firstAt time.Time     // first Observe, for the settling window
	lastAt  time.Time     // previous Observe, for the slew budget
	base    time.Duration // helper time of plugin ns 0 is ref + base
	mins    []clockSample // ascending raw, ascending at: a sliding-window minimum

	highSince time.Time     // first of an unbroken run of estimates above base+step
	highMin   time.Duration // the lowest estimate in that run
}

type clockSample struct {
	at  time.Time
	raw time.Duration
}

func newClockMap(window time.Duration, slewPPM int64, step time.Duration) *clockMap {
	return &clockMap{window: window, slewPPM: slewPPM, step: step}
}

// Observe records one message received at now (helper clock) that the plugin
// stamped sendNs just before writing it.
func (c *clockMap) Observe(now time.Time, sendNs uint64) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if !c.have {
		c.have = true
		c.ref, c.firstAt, c.lastAt = now, now, now
		c.base = -time.Duration(sendNs)
		c.mins = []clockSample{{at: now, raw: c.base}}
		return
	}
	raw := now.Sub(c.ref) - time.Duration(sendNs)
	// Monotonic deque: a newer, lower estimate makes older higher ones irrelevant.
	for len(c.mins) > 0 && c.mins[len(c.mins)-1].raw >= raw {
		c.mins = c.mins[:len(c.mins)-1]
	}
	c.mins = append(c.mins, clockSample{at: now, raw: raw})
	for len(c.mins) > 1 && now.Sub(c.mins[0].at) > c.window {
		c.mins = c.mins[1:]
	}
	if raw > c.base+c.step {
		if c.highSince.IsZero() || raw < c.highMin {
			if c.highSince.IsZero() {
				c.highSince = now
			}
			c.highMin = raw
		}
		if now.Sub(c.highSince) >= clockMapRaise {
			// The clocks' relation moved forward: restart the window there.
			c.base = c.highMin
			c.mins = []clockSample{{at: now, raw: c.highMin}}
			c.highSince = time.Time{}
			c.lastAt = now
			return
		}
	} else {
		c.highSince = time.Time{}
	}
	target := c.mins[0].raw
	elapsed := now.Sub(c.lastAt)
	c.lastAt = now
	diff := target - c.base
	switch {
	case now.Sub(c.firstAt) < c.window, diff > c.step, diff < -c.step:
		c.base = target
	default:
		budget := time.Duration(int64(elapsed) * c.slewPPM / 1_000_000)
		if diff > budget {
			diff = budget
		} else if diff < -budget {
			diff = -budget
		}
		c.base += diff
	}
}

// Map returns the helper time of a plugin timestamp, or the zero time before
// the first Observe.
func (c *clockMap) Map(captureNs uint64) time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	if !c.have {
		return time.Time{}
	}
	return c.ref.Add(c.base + time.Duration(captureNs))
}
