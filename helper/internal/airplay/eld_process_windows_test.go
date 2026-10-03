//go:build windows && (!cgo || !fdk_aac)

package airplay

// eldPIDReaped relies on the ProcessState check on Windows, where a waited
// process leaves no zombie.
func eldPIDReaped(int) error { return nil }
