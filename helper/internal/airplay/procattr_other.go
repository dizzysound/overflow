//go:build !linux

package airplay

import "os/exec"

// setParentDeathSignal is a no-op outside Linux. The OBS helper never launches
// GStreamer; the standalone CLI's children are cleaned up by Stop.
func setParentDeathSignal(cmd *exec.Cmd) {}
