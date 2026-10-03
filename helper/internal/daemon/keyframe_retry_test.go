package daemon

import (
	"testing"
	"time"
)

func TestKeyframeRetryReason(t *testing.T) {
	joined := time.Unix(1000, 0)
	at := func(ms int) time.Time { return joined.Add(time.Duration(ms) * time.Millisecond) }
	cases := []struct {
		name         string
		now          time.Time
		firstSent    bool
		waiting      bool
		waitingSince time.Time
		want         string
	}{
		{"no first frame yet, 400 ms after join", at(400), false, false, time.Time{}, ""},
		{"no first frame 600 ms after join", at(600), false, false, time.Time{}, "join"},
		{"first frame sent, not shedding", at(5000), true, false, time.Time{}, ""},
		{"waiting for an IDR for 300 ms", at(5000), true, true, at(4700), ""},
		{"waiting for an IDR for 600 ms", at(5000), true, true, at(4400), "backlog"},
	}
	for _, c := range cases {
		if got := keyframeRetryReason(c.now, joined, c.firstSent, c.waitingSince, c.waiting); got != c.want {
			t.Errorf("%s: reason %q, want %q", c.name, got, c.want)
		}
	}
}
