package airplay

import (
	"net"
	"testing"
)

func mustCIDR(t *testing.T, s string) *net.IPNet {
	t.Helper()
	_, n, err := net.ParseCIDR(s)
	if err != nil {
		t.Fatalf("ParseCIDR(%q): %v", s, err)
	}
	return n
}

func TestPreferLocalAddressSameSubnetPreferred(t *testing.T) {
	// The field report: a receiver advertises 10.20.0.164 (the church
	// CameraAV network, where the sender is 10.20.0.120/24) and
	// 192.168.1.168 (a second interface with no local route). The
	// same-subnet address must win regardless of advertised order.
	local := []*net.IPNet{mustCIDR(t, "10.20.0.0/24")}

	addrs := []net.IP{net.ParseIP("192.168.1.168"), net.ParseIP("10.20.0.164")}
	got := preferLocalAddress(addrs, local)
	if !got.Equal(net.ParseIP("10.20.0.164")) {
		t.Fatalf("preferLocalAddress = %v, want 10.20.0.164 (same subnet)", got)
	}

	// Order reversed: still the same-subnet address, not "first advertised".
	addrs = []net.IP{net.ParseIP("10.20.0.164"), net.ParseIP("192.168.1.168")}
	got = preferLocalAddress(addrs, local)
	if !got.Equal(net.ParseIP("10.20.0.164")) {
		t.Fatalf("preferLocalAddress = %v, want 10.20.0.164 (same subnet)", got)
	}
}

func TestPreferLocalAddressNoneLocalFallsBackToFirst(t *testing.T) {
	local := []*net.IPNet{mustCIDR(t, "10.20.0.0/24")}
	addrs := []net.IP{net.ParseIP("192.168.1.168"), net.ParseIP("172.16.5.5")}
	got := preferLocalAddress(addrs, local)
	if !got.Equal(net.ParseIP("192.168.1.168")) {
		t.Fatalf("preferLocalAddress = %v, want first-advertised 192.168.1.168", got)
	}
}

func TestPreferLocalAddressAvoidsLinkLocalWhenAlternativeExists(t *testing.T) {
	// No matching local subnet at all; the link-local address is first, but
	// a routable alternative exists and must be preferred over it.
	addrs := []net.IP{net.ParseIP("169.254.10.1"), net.ParseIP("203.0.113.9")}
	got := preferLocalAddress(addrs, nil)
	if !got.Equal(net.ParseIP("203.0.113.9")) {
		t.Fatalf("preferLocalAddress = %v, want 203.0.113.9 (avoid link-local)", got)
	}
}

func TestPreferLocalAddressLinkLocalOnlyIsReturned(t *testing.T) {
	// When link-local is the only option, it is returned rather than nil.
	addrs := []net.IP{net.ParseIP("169.254.10.1")}
	got := preferLocalAddress(addrs, nil)
	if !got.Equal(net.ParseIP("169.254.10.1")) {
		t.Fatalf("preferLocalAddress = %v, want the sole link-local address", got)
	}
}

func TestPreferLocalAddressIgnoresIPv6(t *testing.T) {
	// preferLocalAddress only ever receives IPv4 addresses from callers, but
	// an IPv6 value handed in should not match an IPv4 local network and
	// should not crash Contains().
	local := []*net.IPNet{mustCIDR(t, "10.20.0.0/24")}
	addrs := []net.IP{net.ParseIP("2001:db8::1"), net.ParseIP("10.20.0.5")}
	got := preferLocalAddress(addrs, local)
	if !got.Equal(net.ParseIP("10.20.0.5")) {
		t.Fatalf("preferLocalAddress = %v, want 10.20.0.5 (IPv4 same subnet, ignoring IPv6 entry)", got)
	}
}

func TestPreferLocalAddressEmptyList(t *testing.T) {
	if got := preferLocalAddress(nil, []*net.IPNet{mustCIDR(t, "10.0.0.0/24")}); got != nil {
		t.Fatalf("preferLocalAddress(nil, ...) = %v, want nil", got)
	}
	if got := preferLocalAddress([]net.IP{}, nil); got != nil {
		t.Fatalf("preferLocalAddress([], nil) = %v, want nil", got)
	}
}

func TestPreferLocalAddressDeterministic(t *testing.T) {
	// Same inputs must produce the same output every time: the daemon calls
	// this once per scan, and a result that flip-flopped between scans for
	// an unchanged advertisement would flap the discovered-device identity
	// (AirPlayDevice.IP is the daemon's map key).
	local := []*net.IPNet{mustCIDR(t, "10.20.0.0/24")}
	addrs := []net.IP{net.ParseIP("192.168.1.168"), net.ParseIP("10.20.0.164")}
	first := preferLocalAddress(addrs, local)
	for i := 0; i < 5; i++ {
		if got := preferLocalAddress(addrs, local); !got.Equal(first) {
			t.Fatalf("preferLocalAddress flapped: run %d = %v, want %v", i, got, first)
		}
	}
}
