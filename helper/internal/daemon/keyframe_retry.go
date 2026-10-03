package daemon

import (
	"context"
	"time"

	"doubletake/internal/airplay"
)

// keyframeRetryAfter is how long a display may wait for an IDR (its first
// frame, or the end of a shed) before its keyframe request is treated as lost.
const keyframeRetryAfter = 500 * time.Millisecond

// keyframeRetryInterval is how often a stream checks whether it needs to ask again.
const keyframeRetryInterval = 250 * time.Millisecond

// keyframeRetryReason returns the reason to ask for an IDR again now, or "":
// "join" while the session has sent no video more than keyframeRetryAfter after
// its sink joined, "backlog" while its sink has waited longer than that for an
// IDR after shedding.
func keyframeRetryReason(now, joined time.Time, firstFrameSent bool, waitingSince time.Time, waiting bool) string {
	if !firstFrameSent {
		if now.Sub(joined) > keyframeRetryAfter {
			return "join"
		}
		return ""
	}
	if waiting && now.Sub(waitingSince) > keyframeRetryAfter {
		return "backlog"
	}
	return ""
}

// watchKeyframes re-requests an IDR for one stream while it waits for one,
// until stop is closed or ctx ends. retry is backed off by the caller.
func watchKeyframes(ctx context.Context, stop <-chan struct{}, retry func(deviceID, reason string),
	deviceID string, sink *airplay.BroadcastSink, firstFrameSent <-chan struct{}, joined time.Time) {
	ticker := time.NewTicker(keyframeRetryInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-stop:
			return
		case now := <-ticker.C:
			first := false
			select {
			case <-firstFrameSent:
				first = true
			default:
			}
			since, waiting := sink.WaitingForIDRSince()
			if reason := keyframeRetryReason(now, joined, first, since, waiting); reason != "" {
				retry(deviceID, reason)
			}
		}
	}
}
