//go:build windows

package airplay

import "syscall"

// setTrafficClass sets IP_TOS. Some Windows versions ignore it; policy-based
// QoS is the backstop there.
func setTrafficClass(fd uintptr, tos int) {
	_ = syscall.SetsockoptInt(syscall.Handle(fd), syscall.IPPROTO_IP, syscall.IP_TOS, tos)
}
