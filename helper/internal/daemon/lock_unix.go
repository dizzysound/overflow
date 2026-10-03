//go:build !windows

package daemon

import (
	"errors"
	"fmt"
	"os"
	"syscall"
)

// acquireInstanceLock prevents two daemons from owning different listeners at
// the same socket path. A Unix socket alone is not sufficient for this: unlinking
// its pathname does not stop the process that is already listening on it.
func acquireInstanceLock(socketPath string) (*os.File, error) {
	lockPath := socketPath + ".lock"
	lockFile, err := os.OpenFile(lockPath, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, fmt.Errorf("open daemon lock %s: %w", lockPath, err)
	}
	if err := lockFile.Chmod(0600); err != nil {
		lockFile.Close()
		return nil, fmt.Errorf("chmod daemon lock %s: %w", lockPath, err)
	}
	if err := syscall.Flock(int(lockFile.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		lockFile.Close()
		if errors.Is(err, syscall.EWOULDBLOCK) || errors.Is(err, syscall.EAGAIN) {
			return nil, fmt.Errorf("another doubletake daemon is already running for %s", socketPath)
		}
		return nil, fmt.Errorf("lock daemon instance %s: %w", lockPath, err)
	}
	return lockFile, nil
}

func releaseInstanceLock(lockFile *os.File) {
	if lockFile == nil {
		return
	}
	_ = syscall.Flock(int(lockFile.Fd()), syscall.LOCK_UN)
	_ = lockFile.Close()
}
