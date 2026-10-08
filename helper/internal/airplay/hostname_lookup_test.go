package airplay

import (
	"context"
	"errors"
	"net"
	"testing"
)

// resetHostLookupTestState restores all the package state a host-name
// lookup test can perturb: the swappable lookup function, its cache, and
// the local-network enumeration (via fakeAttachedNetwork in discovery_test.go).
func resetHostLookupTestState(t *testing.T) (restore func()) {
	t.Helper()
	origLookup := lookupHostIPv4
	origIfaces, origAddrs := netInterfaces, netInterfaceAddrs
	resetHostLookupCacheForTest()
	resetLocalAddrCacheForTest()
	return func() {
		lookupHostIPv4 = origLookup
		netInterfaces, netInterfaceAddrs = origIfaces, origAddrs
		resetHostLookupCacheForTest()
		resetLocalAddrCacheForTest()
	}
}

func TestDeviceFromDNSSDHostNameLookupPicksLocalAddress(t *testing.T) {
	defer resetHostLookupTestState(t)()
	fakeAttachedNetwork(t, "10.20.0.5/24")
	resetLocalAddrCacheForTest()

	calls := 0
	lookupHostIPv4 = func(ctx context.Context, host string) ([]net.IP, error) {
		calls++
		if host != "Gallery.local" {
			t.Fatalf("lookupHostIPv4 host = %q, want Gallery.local", host)
		}
		return []net.IP{net.ParseIP("10.20.0.164")}, nil
	}

	dev := deviceFromDNSSD(dnssdService{
		InstanceName: "Gallery._airplay._tcp.local",
		HostName:     "Gallery.local",
		IPv4:         net.IPv4(192, 168, 1, 168),
		Port:         7000,
	})
	if dev == nil {
		t.Fatal("deviceFromDNSSD returned nil")
	}
	if calls != 1 {
		t.Fatalf("lookupHostIPv4 called %d times, want 1", calls)
	}
	if dev.IP != "10.20.0.164" {
		t.Fatalf("IP = %q, want the local address from the host-name lookup", dev.IP)
	}
	if len(dev.IPs) != 2 {
		t.Fatalf("IPs = %v, want both the resolved and looked-up addresses", dev.IPs)
	}
}

func TestDeviceFromDNSSDHostNameLookupErrorKeepsOriginalAddress(t *testing.T) {
	defer resetHostLookupTestState(t)()
	// No attached network is faked, so the resolved address is never
	// "local" and the lookup always runs.

	lookupHostIPv4 = func(ctx context.Context, host string) ([]net.IP, error) {
		return nil, errors.New("mdns: no reply")
	}

	dev := deviceFromDNSSD(dnssdService{
		InstanceName: "Gallery._airplay._tcp.local",
		HostName:     "Gallery.local",
		IPv4:         net.IPv4(192, 168, 1, 168),
		Port:         7000,
	})
	if dev == nil {
		t.Fatal("deviceFromDNSSD returned nil")
	}
	if dev.IP != "192.168.1.168" {
		t.Fatalf("IP = %q, want the original resolved address kept on lookup failure", dev.IP)
	}
	if len(dev.IPs) != 1 {
		t.Fatalf("IPs = %v, want only the original resolved address", dev.IPs)
	}
}

func TestDeviceFromDNSSDSkipsLookupWhenResolvedAddressIsAlreadyLocal(t *testing.T) {
	defer resetHostLookupTestState(t)()
	fakeAttachedNetwork(t, "10.20.0.5/24")
	resetLocalAddrCacheForTest()

	called := false
	lookupHostIPv4 = func(ctx context.Context, host string) ([]net.IP, error) {
		called = true
		return []net.IP{net.ParseIP("10.20.0.200")}, nil
	}

	dev := deviceFromDNSSD(dnssdService{
		InstanceName: "Gallery._airplay._tcp.local",
		HostName:     "Gallery.local",
		IPv4:         net.IPv4(10, 20, 0, 164), // already on the attached subnet
		Port:         7000,
	})
	if dev == nil {
		t.Fatal("deviceFromDNSSD returned nil")
	}
	if called {
		t.Fatal("lookupHostIPv4 was called even though the resolved address was already local")
	}
	if dev.IP != "10.20.0.164" || len(dev.IPs) != 1 {
		t.Fatalf("device = %+v, want the resolved address unchanged", dev)
	}
}

func TestLookupAdditionalIPv4CachesPerHost(t *testing.T) {
	defer resetHostLookupTestState(t)()

	calls := 0
	lookupHostIPv4 = func(ctx context.Context, host string) ([]net.IP, error) {
		calls++
		return []net.IP{net.ParseIP("10.20.0.164")}, nil
	}

	first := lookupAdditionalIPv4("Gallery.local")
	second := lookupAdditionalIPv4("Gallery.local")

	if calls != 1 {
		t.Fatalf("lookupHostIPv4 called %d times for two lookups inside the cache TTL, want 1", calls)
	}
	if len(first) != 1 || len(second) != 1 || !first[0].Equal(second[0]) {
		t.Fatalf("cached result mismatch: first=%v second=%v", first, second)
	}

	// A different host name is not served from the same cache entry.
	if _, ok := func() (net.IP, bool) {
		res := lookupAdditionalIPv4("OtherPanel.local")
		if len(res) == 0 {
			return nil, false
		}
		return res[0], true
	}(); !ok {
		t.Fatalf("lookupAdditionalIPv4 for a distinct host returned nothing")
	}
	if calls != 2 {
		t.Fatalf("lookupHostIPv4 called %d times after a distinct host, want 2", calls)
	}
}
