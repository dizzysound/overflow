//go:build windows

package daemon

import (
	"errors"
	"os"
)

// acquireInstanceLock is unsupported on Windows: the Unix-socket daemon mode is
// not used there. The OBS helper runs the daemon embedded (see RunEmbedded).
func acquireInstanceLock(socketPath string) (*os.File, error) {
	return nil, errors.New("socket daemon mode is not supported on Windows; use overflow-helper")
}

func releaseInstanceLock(lockFile *os.File) {
	if lockFile != nil {
		_ = lockFile.Close()
	}
}
