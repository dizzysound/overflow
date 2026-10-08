package airplay

import (
	"net"
	"strings"
)

// Go mirrors of the windns.h DNS-SD structures (Windows 10, dnsapi.dll). They
// live in a file without a build tag so TestDNSSDStructLayouts can check them
// on any 64-bit platform; only discovery_native_windows.go hands them to the
// OS. Field order and types follow the Microsoft Learn definitions of
// DNS_SERVICE_BROWSE_REQUEST, DNS_SERVICE_RESOLVE_REQUEST, DNS_SERVICE_CANCEL
// and DNS_SERVICE_INSTANCE (user-mode LPWSTR/PWSTR branch, not the MIDL
// DNSSD_RPC_STRING one).

type dnsServiceBrowseRequest struct {
	Version        uint32  // DNS_QUERY_REQUEST_VERSION1 selects pBrowseCallback
	InterfaceIndex uint32  // 0 = all interfaces
	QueryName      *uint16 // "_airplay._tcp.local"
	BrowseCallback uintptr // PDNS_SERVICE_BROWSE_CALLBACK (union with pBrowseCallbackV2)
	QueryContext   uintptr // an integer key, never a Go pointer
}

type dnsServiceResolveRequest struct {
	Version                   uint32  // must be DNS_QUERY_REQUEST_VERSION1
	InterfaceIndex            uint32  // 0 = all interfaces
	QueryName                 *uint16 // instance FQDN, e.g. "Studio._airplay._tcp.local"
	ResolveCompletionCallback uintptr // PDNS_SERVICE_RESOLVE_COMPLETE
	QueryContext              uintptr
}

type dnsServiceCancel struct {
	reserved uintptr // handle owned by the OS; never modified
}

type dnsServiceInstance struct {
	InstanceName   *uint16
	HostName       *uint16
	IP4Address     *uint32   // IP4_ADDRESS*, network byte order
	IP6Address     *[16]byte // IP6_ADDRESS*
	Port           uint16
	Priority       uint16
	Weight         uint16
	PropertyCount  uint32
	Keys           **uint16 // PWSTR[PropertyCount]
	Values         **uint16 // PWSTR[PropertyCount]
	InterfaceIndex uint32
}

// dnssdService is a resolved DNS-SD instance copied into Go memory.
type dnssdService struct {
	InstanceName string // FQDN
	HostName     string
	IPv4         net.IP // nil when absent
	IPv6         net.IP // nil when absent
	Port         uint16
	Keys         []string // TXT keys, in record order
	Values       []string // TXT values, parallel to Keys
}

const airplayServiceSuffix = "._airplay._tcp.local"

// dnssdInstanceLabel returns the instance label of a DNS-SD service FQDN
// ("Studio._airplay._tcp.local" -> "Studio"), removing any escapes.
func dnssdInstanceLabel(fqdn string) string {
	name := strings.TrimSuffix(fqdn, ".")
	if n := len(name) - len(airplayServiceSuffix); n >= 0 && strings.EqualFold(name[n:], airplayServiceSuffix) {
		name = name[:n]
	}
	return unescapeDNSName(name)
}

// deviceFromDNSSD converts a resolved instance with the same TXT rules as the
// zeroconf path (case-insensitive keys via parseTXT). It prefers IPv4, accepts
// a routable IPv6 address, and returns nil when there is no usable address or
// port.
//
// The Windows DNS_SERVICE_INSTANCE (windns.h) carries a single IP4Address
// pointer and a single IP6Address pointer per resolved instance (see the Go
// mirror above) — DnsServiceResolve exposes at most one address per address
// family, not a full A-record set. When that one IPv4 address is not on a
// network this host is directly attached to, this looks up the instance's
// host name (svc.HostName, DNS_SERVICE_INSTANCE pszHostName, e.g.
// "Gallery.local") for additional IPv4 addresses via lookupAdditionalIPv4
// (hostname_lookup.go), so preferLocalAddress has more than the one resolved
// address to choose from. When the resolved address is already local, the
// lookup is skipped entirely.
func deviceFromDNSSD(svc dnssdService) *AirPlayDevice {
	var ip net.IP
	var ips []net.IP
	switch {
	case svc.IPv4 != nil && !svc.IPv4.IsUnspecified():
		ips = []net.IP{svc.IPv4}
		if !isAddressLocal(svc.IPv4) {
			if extra := lookupAdditionalIPv4(svc.HostName); len(extra) > 0 {
				ips = dedupeIPv4(append(ips, extra...))
			}
		}
		ip = selectDialAddress(dnssdInstanceLabel(svc.InstanceName), ips)
	case svc.IPv6 != nil && !svc.IPv6.IsUnspecified() && !svc.IPv6.IsLinkLocalUnicast():
		ip = svc.IPv6
	default:
		return nil
	}
	if svc.Port == 0 {
		return nil
	}
	records := make([]string, 0, len(svc.Keys))
	for i, key := range svc.Keys {
		if key == "" {
			continue
		}
		value := ""
		if i < len(svc.Values) {
			value = svc.Values[i]
		}
		records = append(records, key+"="+value)
	}
	dev := &AirPlayDevice{Name: dnssdInstanceLabel(svc.InstanceName), IP: ip.String(), IPs: ips, Port: int(svc.Port)}
	populateDeviceFromTXT(dev, parseTXT(records))
	return dev
}

// ipv4FromIP4Address converts the bytes of an IP4_ADDRESS, which holds the
// address in network byte order.
func ipv4FromIP4Address(b [4]byte) net.IP {
	return net.IPv4(b[0], b[1], b[2], b[3])
}
