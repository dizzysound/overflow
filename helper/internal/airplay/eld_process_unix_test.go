//go:build !windows && (!cgo || !fdk_aac)

package airplay

import (
	"errors"
	"fmt"
	"syscall"
)

// eldPIDReaped checks that Wait reaped the child: its PID no longer exists,
// so no zombie remains.
func eldPIDReaped(pid int) error {
	if err := syscall.Kill(pid, 0); !errors.Is(err, syscall.ESRCH) {
		return fmt.Errorf("kill(%d, 0) after Close = %v, want ESRCH", pid, err)
	}
	return nil
}
