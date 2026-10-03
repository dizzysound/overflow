//go:build unix

package airplay

import "syscall"

// setTrafficClass sets the IPv4 TOS and IPv6 traffic class bytes; a
// dual-stack socket needs both, and one of them fails on single-stack sockets.
func setTrafficClass(fd uintptr, tos int) {
	_ = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IP, syscall.IP_TOS, tos)
	_ = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IPV6, syscall.IPV6_TCLASS, tos)
}
