//go:build !linux && !windows

package airplay

import "time"

// Non-Linux platforms retain the process-relative monotonic fallback.
func bootRelativeNow() time.Duration {
	return time.Since(appStartTime)
}
