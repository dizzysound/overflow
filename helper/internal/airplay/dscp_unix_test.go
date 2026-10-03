//go:build unix

package airplay

import (
	"net"
	"syscall"
	"testing"
)

func TestMarkDSCPSetsIPv4TOS(t *testing.T) {
	conn, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	markDSCP(conn, dscpAudio)
	raw, err := conn.(*net.UDPConn).SyscallConn()
	if err != nil {
		t.Fatal(err)
	}
	var tos int
	var getErr error
	if err := raw.Control(func(fd uintptr) { tos, getErr = syscall.GetsockoptInt(int(fd), syscall.IPPROTO_IP, syscall.IP_TOS) }); err != nil {
		t.Fatal(err)
	}
	if getErr != nil {
		t.Fatal(getErr)
	}
	if tos != dscpAudio<<2 {
		t.Fatalf("IP_TOS = 0x%x, want 0x%x (EF)", tos, dscpAudio<<2)
	}
}

func TestConsecutiveUDPPortsAreMarked(t *testing.T) {
	conns, ok := tryConsecutiveUDP(0, 2)
	defer func() {
		for _, c := range conns {
			c.Close()
		}
	}()
	if !ok {
		t.Skip("no two consecutive free UDP ports")
	}
	for i, c := range conns {
		raw, err := c.(*net.UDPConn).SyscallConn()
		if err != nil {
			t.Fatal(err)
		}
		var tclass int
		_ = raw.Control(func(fd uintptr) {
			tclass, _ = syscall.GetsockoptInt(int(fd), syscall.IPPROTO_IPV6, syscall.IPV6_TCLASS)
		})
		if tclass != dscpAudio<<2 {
			t.Fatalf("socket %d IPV6_TCLASS = 0x%x, want 0x%x", i, tclass, dscpAudio<<2)
		}
	}
}
