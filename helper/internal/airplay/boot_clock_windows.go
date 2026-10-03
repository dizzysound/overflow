//go:build windows

package airplay

import (
	"time"

	"golang.org/x/sys/windows"
)

// bootUptimeAtStart is the system uptime when the process started. Linux uses
// CLOCK_BOOTTIME, so NTP-mode timestamps there count from boot (typically days).
// Windows previously fell back to time since process start, which begins near
// zero; UxPlay-based receivers stalled on that clock while Apple TV and Roku did
// not. DurationSinceBoot (GetTickCount64) has millisecond resolution, so it only sets the base;
// Go's monotonic clock supplies the precision.
var bootUptimeAtStart = windows.DurationSinceBoot()

func bootRelativeNow() time.Duration {
	return bootUptimeAtStart + time.Since(appStartTime)
}
