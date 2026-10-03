package airplay

import (
	"net"
	"syscall"
)

// DSCP classes for AirPlay media, so switches and Wi-Fi access points queue it
// ahead of bulk traffic (as NDI senders do). On Wi-Fi the access point maps the
// class to a WMM access category: EF to voice (RFC 8325) or video on
// precedence-based mappings, AF41 to video.
const (
	dscpAudio = 46 // EF: audio data, audio control (resends) and timing
	dscpVideo = 34 // AF41: the video data stream
)

// markDSCP sets conn's outgoing DSCP class. It is best effort: an error leaves
// the socket unmarked. Windows can ignore a socket's own marking (it reached the
// wire on the streaming PC, 2026-10-01); a policy-based QoS rule
// (New-NetQosPolicy) covering the helper is the backstop. See docs/protocol.md.
func markDSCP(conn any, dscp int) {
	sc, ok := conn.(syscall.Conn)
	if !ok {
		return
	}
	raw, err := sc.SyscallConn()
	if err != nil {
		return
	}
	_ = raw.Control(func(fd uintptr) { setTrafficClass(fd, dscp<<2) })
}

var (
	_ syscall.Conn = (*net.UDPConn)(nil)
	_ syscall.Conn = (*net.TCPConn)(nil)
)
