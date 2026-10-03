package bridge

import (
	"sync"
	"sync/atomic"
	"time"
)

// keyframeThrottle limits keyframe events to one per interval without losing
// requests. The first request in a quiet period emits immediately (leading
// edge). Requests inside the window are coalesced into one trailing emit at the
// window's end, carrying the most recent request's device and reason; that emit
// starts a new window. With one shared encoder one IDR serves every display, so
// coalescing loses nothing, but dropping a late request would leave a shed
// display waiting for the next scheduled IDR.
type keyframeThrottle struct {
	mu       sync.Mutex
	interval time.Duration
	emit     func(deviceID, reason string)
	last     time.Time
	timer    *time.Timer
	pending  bool
	pendID   string
	pendWhy  string
	stopped  bool
}

func newKeyframeThrottle(interval time.Duration, emit func(deviceID, reason string)) *keyframeThrottle {
	return &keyframeThrottle{interval: interval, emit: emit}
}

// request never blocks beyond the emit callback itself.
func (k *keyframeThrottle) request(deviceID, reason string) {
	now := time.Now()
	k.mu.Lock()
	if k.stopped {
		k.mu.Unlock()
		return
	}
	if k.last.IsZero() || now.Sub(k.last) >= k.interval {
		k.last = now
		k.mu.Unlock()
		k.emit(deviceID, reason)
		return
	}
	k.pending, k.pendID, k.pendWhy = true, deviceID, reason
	if k.timer == nil {
		k.timer = time.AfterFunc(k.last.Add(k.interval).Sub(now), k.fire)
	}
	k.mu.Unlock()
}

func (k *keyframeThrottle) fire() {
	k.mu.Lock()
	k.timer = nil
	if k.stopped || !k.pending {
		k.mu.Unlock()
		return
	}
	id, why := k.pendID, k.pendWhy
	k.pending = false
	k.last = time.Now()
	k.mu.Unlock()
	k.emit(id, why)
}

// stop cancels any pending trailing emit; later requests are ignored.
func (k *keyframeThrottle) stop() {
	k.mu.Lock()
	defer k.mu.Unlock()
	k.stopped = true
	k.pending = false
	if k.timer != nil {
		k.timer.Stop()
		k.timer = nil
	}
}

// Per-display backoff for keyframe requests from a display that keeps shedding
// (reason "backlog", and every re-request while a display waits for an IDR).
// Each forced IDR costs every TV bitrate, so one slow display must not force
// them continuously. "join" and "receiver" requests skip it.
const (
	keyframeBackoffMin   = time.Second
	keyframeBackoffMax   = 4 * time.Second
	keyframeBackoffReset = 10 * time.Second
)

type keyframeBackoff struct {
	mu       sync.Mutex
	now      func() time.Time
	displays map[string]*keyframeBackoffState
}

type keyframeBackoffState struct {
	next     time.Time     // earliest time the next request may pass
	gap      time.Duration // the gap applied after the last allowed request
	lastShed time.Time     // the last request, allowed or not
}

func newKeyframeBackoff(now func() time.Time) *keyframeBackoff {
	return &keyframeBackoff{now: now, displays: map[string]*keyframeBackoffState{}}
}

// allow reports whether deviceID's request may go on to the throttle now. The
// first is allowed at once, then at least 1 s apart, doubling to 4 s while the
// display keeps asking; 10 s without a request starts over.
func (b *keyframeBackoff) allow(deviceID string) bool {
	now := b.now()
	b.mu.Lock()
	defer b.mu.Unlock()
	st := b.displays[deviceID]
	if st == nil || now.Sub(st.lastShed) >= keyframeBackoffReset {
		st = &keyframeBackoffState{}
		b.displays[deviceID] = st
	}
	st.lastShed = now
	if !st.next.IsZero() && now.Before(st.next) {
		return false
	}
	switch {
	case st.gap == 0:
		st.gap = keyframeBackoffMin
	case st.gap*2 > keyframeBackoffMax:
		st.gap = keyframeBackoffMax
	default:
		st.gap *= 2
	}
	st.next = now.Add(st.gap)
	return true
}

// keyframeSlowAfter is when a request-to-IDR time is logged as a warning: the
// long NVENC GOP is safe only while forced IDRs arrive well within it.
const keyframeSlowAfter = time.Second

// keyframeLatencyMaxPending bounds the requests remembered while no IDR comes.
const keyframeLatencyMaxPending = 32

// keyframeLatency logs, once per emitted keyframe event, how long the next
// IDR access unit took to reach the broadcast.
type keyframeLatency struct {
	mu      sync.Mutex
	now     func() time.Time
	logf    func(format string, args ...any)
	pending []keyframeRequest
	waiting atomic.Bool
}

type keyframeRequest struct {
	deviceID, reason string
	at               time.Time
}

func newKeyframeLatency(now func() time.Time, logf func(format string, args ...any)) *keyframeLatency {
	return &keyframeLatency{now: now, logf: logf}
}

func (l *keyframeLatency) requested(deviceID, reason string) {
	now := l.now()
	l.mu.Lock()
	if len(l.pending) < keyframeLatencyMaxPending {
		l.pending = append(l.pending, keyframeRequest{deviceID, reason, now})
	}
	l.waiting.Store(true)
	l.mu.Unlock()
}

// wantIDR is the broadcast's per-access-unit check: one atomic load.
func (l *keyframeLatency) wantIDR() bool { return l.waiting.Load() }

func (l *keyframeLatency) sawIDR() {
	now := l.now()
	l.mu.Lock()
	done := l.pending
	l.pending = nil
	l.waiting.Store(false)
	l.mu.Unlock()
	for _, r := range done {
		d := now.Sub(r.at)
		if d > keyframeSlowAfter {
			l.logf("[keyframe] WARNING: requested (%s, %s): IDR after %dms, more than 1 s", r.reason, r.deviceID, d.Milliseconds())
		} else {
			l.logf("[keyframe] requested (%s, %s): IDR after %dms", r.reason, r.deviceID, d.Milliseconds())
		}
	}
}
