package bridge

import (
	"fmt"
	"sync"
	"testing"
	"time"
)

type kfRecorder struct {
	mu  sync.Mutex
	got []string
}

func (r *kfRecorder) emit(id, reason string) {
	r.mu.Lock()
	r.got = append(r.got, id+":"+reason)
	r.mu.Unlock()
}

func (r *kfRecorder) snapshot() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]string(nil), r.got...)
}

func eq(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

const kfWindow = 150 * time.Millisecond

func TestKeyframeThrottleLeadingEdgeImmediate(t *testing.T) {
	r := &kfRecorder{}
	th := newKeyframeThrottle(kfWindow, r.emit)
	defer th.stop()
	th.request("A", "join")
	if got := r.snapshot(); !eq(got, []string{"A:join"}) {
		t.Fatalf("emits = %v, want immediate A:join", got)
	}
	time.Sleep(kfWindow + 100*time.Millisecond)
	th.request("B", "backlog") // quiet period passed: leading edge again
	if got := r.snapshot(); !eq(got, []string{"A:join", "B:backlog"}) {
		t.Fatalf("emits = %v", got)
	}
}

func TestKeyframeThrottleTrailingEdgeCarriesLastRequest(t *testing.T) {
	r := &kfRecorder{}
	th := newKeyframeThrottle(kfWindow, r.emit)
	defer th.stop()
	th.request("A", "join")
	time.Sleep(30 * time.Millisecond)
	th.request("B", "backlog")
	if got := r.snapshot(); !eq(got, []string{"A:join"}) {
		t.Fatalf("before window end emits = %v, want only the leading one", got)
	}
	time.Sleep(kfWindow + 100*time.Millisecond)
	if got := r.snapshot(); !eq(got, []string{"A:join", "B:backlog"}) {
		t.Fatalf("emits = %v, want leading then one trailing B:backlog", got)
	}
}

func TestKeyframeThrottleThreeRequestsOneTrailing(t *testing.T) {
	r := &kfRecorder{}
	th := newKeyframeThrottle(kfWindow, r.emit)
	defer th.stop()
	th.request("A", "join")
	th.request("B", "backlog")
	th.request("C", "receiver")
	time.Sleep(kfWindow + 100*time.Millisecond)
	if got := r.snapshot(); !eq(got, []string{"A:join", "C:receiver"}) {
		t.Fatalf("emits = %v, want A:join then one trailing C:receiver", got)
	}
}

func TestKeyframeThrottleNoTrailingWithoutThrottledRequest(t *testing.T) {
	r := &kfRecorder{}
	th := newKeyframeThrottle(kfWindow, r.emit)
	defer th.stop()
	th.request("A", "join")
	time.Sleep(2*kfWindow + 100*time.Millisecond)
	if got := r.snapshot(); !eq(got, []string{"A:join"}) {
		t.Fatalf("emits = %v, want only the leading one", got)
	}
}

func TestKeyframeThrottleStopCancelsTrailing(t *testing.T) {
	r := &kfRecorder{}
	th := newKeyframeThrottle(kfWindow, r.emit)
	th.request("A", "join")
	th.request("B", "backlog")
	th.stop()
	time.Sleep(kfWindow + 100*time.Millisecond)
	if got := r.snapshot(); !eq(got, []string{"A:join"}) {
		t.Fatalf("emits = %v, want no emit after stop", got)
	}
}

type fakeClock struct{ t time.Time }

func (c *fakeClock) now() time.Time          { return c.t }
func (c *fakeClock) advance(d time.Duration) { c.t = c.t.Add(d) }

func TestKeyframeBackoffDoublesToFourSecondsAndResetsAfterQuiet(t *testing.T) {
	c := &fakeClock{t: time.Unix(1000, 0)}
	b := newKeyframeBackoff(c.now)
	if !b.allow("A") {
		t.Fatal("first request not allowed immediately")
	}
	c.advance(500 * time.Millisecond)
	if b.allow("A") {
		t.Fatal("second request allowed 500 ms after the first, want at least 1 s apart")
	}
	if !b.allow("B") {
		t.Fatal("another display is backed off by A's shedding")
	}
	c.advance(500 * time.Millisecond) // t = 1 s
	if !b.allow("A") {
		t.Fatal("request 1 s after the first not allowed")
	}
	c.advance(1500 * time.Millisecond) // t = 2.5 s: gap is now 2 s
	if b.allow("A") {
		t.Fatal("request 1.5 s after the second allowed, want 2 s")
	}
	c.advance(500 * time.Millisecond) // t = 3 s
	if !b.allow("A") {
		t.Fatal("request 2 s after the second not allowed")
	}
	c.advance(3 * time.Second) // t = 6 s: gap is now 4 s
	if b.allow("A") {
		t.Fatal("request 3 s after the third allowed, want 4 s")
	}
	c.advance(time.Second) // t = 7 s
	if !b.allow("A") {
		t.Fatal("request 4 s after the third not allowed")
	}
	c.advance(4 * time.Second) // t = 11 s: stays at the 4 s maximum
	if !b.allow("A") {
		t.Fatal("request 4 s after the fourth not allowed (max 4 s)")
	}
	c.advance(10 * time.Second) // 10 s without a shed: reset
	if !b.allow("A") {
		t.Fatal("request after 10 s quiet not allowed")
	}
	c.advance(time.Second)
	if !b.allow("A") {
		t.Fatal("after a reset the gap is 1 s again")
	}
}

func TestKeyframeBackoffDoesNotResetBeforeTenQuietSeconds(t *testing.T) {
	c := &fakeClock{t: time.Unix(1000, 0)}
	b := newKeyframeBackoff(c.now)
	b.allow("A")                       // gap 1 s
	c.advance(9900 * time.Millisecond) // under 10 s quiet
	if !b.allow("A") {
		t.Fatal("request 9.9 s later not allowed")
	}
	c.advance(time.Second) // gap is 2 s now, not reset to 1 s
	if b.allow("A") {
		t.Fatal("backoff reset after only 9.9 s without a shed")
	}
}

func TestKeyframeLatencyLogsOneLinePerRequestAtTheNextIDR(t *testing.T) {
	c := &fakeClock{t: time.Unix(1000, 0)}
	var lines []string
	l := newKeyframeLatency(c.now, func(format string, args ...any) { lines = append(lines, fmt.Sprintf(format, args...)) })
	if l.wantIDR() {
		t.Fatal("wants an IDR before any request")
	}
	l.requested("AA:BB", "join")
	c.advance(10 * time.Millisecond)
	l.requested("CC:DD", "backlog")
	if !l.wantIDR() {
		t.Fatal("does not want an IDR after a request")
	}
	c.advance(24 * time.Millisecond)
	l.sawIDR()
	want := []string{
		"[keyframe] requested (join, AA:BB): IDR after 34ms",
		"[keyframe] requested (backlog, CC:DD): IDR after 24ms",
	}
	if !eq(lines, want) {
		t.Fatalf("lines = %q, want %q", lines, want)
	}
	if l.wantIDR() {
		t.Fatal("still wants an IDR after one arrived")
	}
	l.sawIDR() // nothing pending: no line
	l.requested("AA:BB", "receiver")
	c.advance(1200 * time.Millisecond)
	l.sawIDR()
	if len(lines) != 3 || lines[2] != "[keyframe] WARNING: requested (receiver, AA:BB): IDR after 1200ms, more than 1 s" {
		t.Fatalf("slow IDR line = %q", lines[len(lines)-1])
	}
}
