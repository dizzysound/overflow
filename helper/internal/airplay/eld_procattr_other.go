//go:build !windows && (!cgo || !fdk_aac)

package airplay

import "os/exec"

// configureELDEncoderCommand needs nothing outside Windows. If the helper dies,
// the encoder sees EOF on stdin and exits on its own.
func configureELDEncoderCommand(cmd *exec.Cmd) {}
