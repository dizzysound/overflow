package airplay

import (
	"net"
	"testing"
	"unsafe"
)

func TestDNSSDInstanceLabel(t *testing.T) {
	for _, tc := range []struct{ in, want string }{
		{"Sacristy._airplay._tcp.local", "Sacristy"},
		{"Sacristy._airplay._tcp.local.", "Sacristy"},
		{`Living\ Room._AirPlay._TCP.local`, "Living Room"},
		{"Hisense Roku TV._airplay._tcp.local", "Hisense Roku TV"},
		{"no-suffix", "no-suffix"},
	} {
		if got := dnssdInstanceLabel(tc.in); got != tc.want {
			t.Errorf("dnssdInstanceLabel(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

func TestDeviceFromDNSSDParsesTXTCaseInsensitively(t *testing.T) {
	dev := deviceFromDNSSD(dnssdService{
		InstanceName: "Friendship._airplay._tcp.local",
		IPv4:         net.IPv4(192, 168, 1, 58),
		Port:         7000,
		Keys:         []string{"deviceId", "model", "features", "srcvers", "flag"},
		Values:       []string{"4a:8a:5a:ea:01:8a", "AppleTV3,2", "0x1,0x2", "220.68", ""},
	})
	if dev == nil {
		t.Fatal("deviceFromDNSSD returned nil")
	}
	if dev.Name != "Friendship" || dev.IP != "192.168.1.58" || dev.Port != 7000 {
		t.Fatalf("identity = %q %s:%d", dev.Name, dev.IP, dev.Port)
	}
	if dev.DeviceID != "4a:8a:5a:ea:01:8a" || dev.Model != "AppleTV3,2" || dev.SourceVersion != "220.68" {
		t.Fatalf("TXT fields = %+v", dev)
	}
	if dev.Features != 2<<32|1 {
		t.Fatalf("Features = %#x, want %#x", dev.Features, uint64(2<<32|1))
	}
	if _, ok := dev.RawTXT["deviceid"]; !ok {
		t.Fatalf("RawTXT keys not lowercased: %v", dev.RawTXT)
	}
	if v, ok := dev.RawTXT["flag"]; !ok || v != "" {
		t.Fatalf("empty-valued key lost: %v", dev.RawTXT)
	}
}

func TestDeviceFromDNSSDAddressChoice(t *testing.T) {
	both := deviceFromDNSSD(dnssdService{IPv4: net.IPv4(10, 20, 0, 164), IPv6: net.ParseIP("2001:db8::1"), Port: 7000})
	if both == nil || both.IP != "10.20.0.164" {
		t.Fatalf("IPv4 not preferred: %+v", both)
	}
	v6 := deviceFromDNSSD(dnssdService{IPv6: net.ParseIP("2001:db8::1"), Port: 7000})
	if v6 == nil || v6.IP != "2001:db8::1" {
		t.Fatalf("IPv6 fallback: %+v", v6)
	}
	for name, svc := range map[string]dnssdService{
		"no address":        {Port: 7000},
		"unspecified":       {IPv4: net.IPv4zero, Port: 7000},
		"link-local only":   {IPv6: net.ParseIP("fe80::1"), Port: 7000},
		"zero port":         {IPv4: net.IPv4(10, 0, 0, 5)},
		"mismatched values": {IPv4: net.IPv4(10, 0, 0, 5), Port: 7000, Keys: []string{"deviceid"}},
	} {
		dev := deviceFromDNSSD(svc)
		if name == "mismatched values" {
			if dev == nil || dev.DeviceID != "" {
				t.Errorf("%s: %+v, want a device with an empty deviceid", name, dev)
			}
			continue
		}
		if dev != nil {
			t.Errorf("%s: got %+v, want nil", name, dev)
		}
	}
}

// TestDeviceFromDNSSDPopulatesSingleAddressIPs documents the native Windows
// backend's actual capability: DNS_SERVICE_INSTANCE (windns.h) carries one
// IP4Address pointer per resolved instance, not an array, so this backend can
// only ever contribute a single-element IPs slice. See the comment on
// deviceFromDNSSD and the dnsServiceInstance mirror above it in dnssd.go.
func TestDeviceFromDNSSDPopulatesSingleAddressIPs(t *testing.T) {
	dev := deviceFromDNSSD(dnssdService{IPv4: net.IPv4(10, 20, 0, 164), Port: 7000})
	if dev == nil {
		t.Fatal("deviceFromDNSSD returned nil")
	}
	if len(dev.IPs) != 1 || !dev.IPs[0].Equal(net.IPv4(10, 20, 0, 164)) {
		t.Fatalf("IPs = %v, want exactly the one resolved IPv4 address", dev.IPs)
	}

	// The IPv6-only path has no IPv4 address to offer preferLocalAddress at
	// all, so IPs stays empty rather than smuggling an IPv6 value into an
	// IPv4-only slice.
	v6 := deviceFromDNSSD(dnssdService{IPv6: net.ParseIP("2001:db8::1"), Port: 7000})
	if v6 == nil || len(v6.IPs) != 0 {
		t.Fatalf("IPv6-only device should have empty IPs, got %+v", v6)
	}
}

func TestIPv4FromIP4Address(t *testing.T) {
	// IP4_ADDRESS holds the address in network byte order, so its bytes in
	// memory are the dotted quad in order.
	if got := ipv4FromIP4Address([4]byte{10, 20, 0, 178}); got.String() != "10.20.0.178" {
		t.Fatalf("got %s", got)
	}
}

// TestDNSSDStructLayouts pins the Go mirrors of the windns.h structures to the
// C layout on 64-bit Windows (offsets from the struct definitions cited in the
// plan). It runs on every 64-bit platform because the layout is pure Go.
func TestDNSSDStructLayouts(t *testing.T) {
	if unsafe.Sizeof(uintptr(0)) != 8 {
		t.Skip("layouts are asserted for 64-bit targets")
	}
	var inst dnsServiceInstance
	var browse dnsServiceBrowseRequest
	var resolve dnsServiceResolveRequest
	for _, c := range []struct {
		name      string
		got, want uintptr
	}{
		{"sizeof(DNS_SERVICE_INSTANCE)", unsafe.Sizeof(inst), 72},
		{"DNS_SERVICE_INSTANCE.ip4Address", unsafe.Offsetof(inst.IP4Address), 16},
		{"DNS_SERVICE_INSTANCE.wPort", unsafe.Offsetof(inst.Port), 32},
		{"DNS_SERVICE_INSTANCE.dwPropertyCount", unsafe.Offsetof(inst.PropertyCount), 40},
		{"DNS_SERVICE_INSTANCE.keys", unsafe.Offsetof(inst.Keys), 48},
		{"DNS_SERVICE_INSTANCE.values", unsafe.Offsetof(inst.Values), 56},
		{"DNS_SERVICE_INSTANCE.dwInterfaceIndex", unsafe.Offsetof(inst.InterfaceIndex), 64},
		{"sizeof(DNS_SERVICE_BROWSE_REQUEST)", unsafe.Sizeof(browse), 32},
		{"DNS_SERVICE_BROWSE_REQUEST.QueryName", unsafe.Offsetof(browse.QueryName), 8},
		{"DNS_SERVICE_BROWSE_REQUEST.pBrowseCallback", unsafe.Offsetof(browse.BrowseCallback), 16},
		{"DNS_SERVICE_BROWSE_REQUEST.pQueryContext", unsafe.Offsetof(browse.QueryContext), 24},
		{"sizeof(DNS_SERVICE_RESOLVE_REQUEST)", unsafe.Sizeof(resolve), 32},
		{"DNS_SERVICE_RESOLVE_REQUEST.pResolveCompletionCallback", unsafe.Offsetof(resolve.ResolveCompletionCallback), 16},
		{"sizeof(DNS_SERVICE_CANCEL)", unsafe.Sizeof(dnsServiceCancel{}), 8},
	} {
		if c.got != c.want {
			t.Errorf("%s = %d, want %d", c.name, c.got, c.want)
		}
	}
}
