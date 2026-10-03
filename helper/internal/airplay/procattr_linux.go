//go:build linux

package airplay

import (
	"os/exec"
	"syscall"
)

// setParentDeathSignal makes the kernel kill cmd if doubletake exits first.
func setParentDeathSignal(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
}
