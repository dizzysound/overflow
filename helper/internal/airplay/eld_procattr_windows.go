//go:build windows && (!cgo || !fdk_aac)

package airplay

import (
	"os/exec"
	"syscall"
)

// createNoWindow is CREATE_NO_WINDOW: eld-encoder is a console program, and
// without this flag a helper that has no console of its own would make
// Windows open a visible console window for it.
const createNoWindow = 0x08000000

func configureELDEncoderCommand(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
}
