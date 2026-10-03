package airplay

import (
	"log"
	"net"
	"sync"
	"time"
)

// preferLocalAddress chooses which of a device's advertised IPv4 addresses to
// dial. It prefers an address inside one of the host's directly attached
// IPv4 networks; otherwise it falls back to the first advertised address
// (the existing behavior before multi-address support). It never returns a
// 169.254/16 link-local address when a non-link-local alternative exists.
//
// This matters on networks where a receiver has two interfaces on different
// subnets (e.g. a UxPlay panel bridging Ethernet and Wi-Fi) and mDNS/DNS-SD
// advertises both: dialing the address on the subnet the sender is actually
// attached to avoids routing the stream through an intermediate gateway that
// may not sustain it (see the Newline/Friendship field report, 2026-09-26).
//
// preferLocalAddress is a pure function so it can be unit tested without any
// real network interfaces.
func preferLocalAddress(addrs []net.IP, localNets []*net.IPNet) net.IP {
	if len(addrs) == 0 {
		return nil
	}

	fallback := addrs[0]
	if isLinkLocalIPv4(fallback) {
		for _, a := range addrs {
			if !isLinkLocalIPv4(a) {
				fallback = a
				break
			}
		}
	}

	for _, a := range addrs {
		for _, n := range localNets {
			if n != nil && n.Contains(a) {
				return a
			}
		}
	}

	return fallback
}

func isLinkLocalIPv4(ip net.IP) bool {
	return ip != nil && ip.IsLinkLocalUnicast()
}

// localAddrCacheTTL bounds how long localIPv4Networks() reuses a previous
// enumeration. The host's addresses can change (DHCP renewal, an interface
// coming up or down), so this is a brief cache, not a snapshot for the life
// of the process.
const localAddrCacheTTL = 5 * time.Second

var (
	localAddrMu       sync.Mutex
	localAddrCached   []*net.IPNet
	localAddrCachedAt time.Time

	// Indirections for tests.
	netInterfaces     = net.Interfaces
	netInterfaceAddrs = func(iface net.Interface) ([]net.Addr, error) { return iface.Addrs() }
)

// localIPv4Networks enumerates the IPv4 networks the host is directly
// attached to, from net.Interfaces()/Addrs(), excluding loopback, link-local
// (169.254/16), and interfaces that are down. The result is cached briefly
// (localAddrCacheTTL) since this runs once per discovered device per scan.
func localIPv4Networks() []*net.IPNet {
	localAddrMu.Lock()
	defer localAddrMu.Unlock()

	if time.Since(localAddrCachedAt) < localAddrCacheTTL {
		return localAddrCached
	}

	ifaces, err := netInterfaces()
	if err != nil {
		return localAddrCached // stale is better than nothing
	}

	var nets []*net.IPNet
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		if iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := netInterfaceAddrs(iface)
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			ipNet, ok := addr.(*net.IPNet)
			if !ok {
				continue
			}
			ip4 := ipNet.IP.To4()
			if ip4 == nil || ip4.IsLoopback() || ip4.IsLinkLocalUnicast() {
				continue
			}
			nets = append(nets, &net.IPNet{IP: ip4.Mask(ipNet.Mask), Mask: ipNet.Mask})
		}
	}

	localAddrCached = nets
	localAddrCachedAt = time.Now()
	return nets
}

// resetLocalAddrCacheForTest forces the next localIPv4Networks() call to
// re-enumerate. Test-only.
func resetLocalAddrCacheForTest() {
	localAddrMu.Lock()
	localAddrCached = nil
	localAddrCachedAt = time.Time{}
	localAddrMu.Unlock()
}

// addressChoiceState tracks, per device name, whether the "dialing a
// non-first-advertised address" info line has already been logged, so it is
// emitted once per device rather than once per scan.
var (
	addressChoiceMu    sync.Mutex
	addressChoiceNoted = map[string]bool{}
)

// selectDialAddress picks the IPv4 address to dial from those a device
// advertised (addrs must be non-empty) and logs the choice: a debug line
// whenever more than one address was available, and a one-time info line per
// device name when the chosen address differs from the first one advertised.
func selectDialAddress(name string, addrs []net.IP) net.IP {
	chosen := preferLocalAddress(addrs, localIPv4Networks())
	if len(addrs) <= 1 {
		return chosen
	}

	reason := "no attached subnet match, using first advertised"
	for _, n := range localIPv4Networks() {
		if n != nil && n.Contains(chosen) {
			reason = "same subnet"
			break
		}
	}
	dbg("[discovery] %q advertises %v, using %s (%s)", name, addrs, chosen, reason)

	if !chosen.Equal(addrs[0]) {
		addressChoiceMu.Lock()
		already := addressChoiceNoted[name]
		addressChoiceNoted[name] = true
		addressChoiceMu.Unlock()
		if !already {
			log.Printf("[discovery] %q: dialing %s instead of the first-advertised %s (%s)",
				name, chosen, addrs[0], reason)
		}
	}
	return chosen
}
