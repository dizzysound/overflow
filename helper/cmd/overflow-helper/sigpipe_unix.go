//go:build !windows

package main

import (
	"os/signal"
	"syscall"
)

// A write to a closed stdout or stderr pipe raises SIGPIPE, which by default
// kills a Go program writing to fd 1 or 2. Ignore it so the write returns
// EPIPE and the helper follows its normal shutdown path.
func init() {
	signal.Ignore(syscall.SIGPIPE)
}
