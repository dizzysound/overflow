package airplay

import (
	"context"
	"net"
	"sync"
	"time"
)

// Host-name address lookup for the Windows native DNS-SD backend.
//
// DnsServiceResolve's DNS_SERVICE_INSTANCE exposes a single IP4Address, not
// an array (see the windns.h citation on deviceFromDNSSD in dnssd.go). When
// that one address is not on a network this host is directly attached to —
// the Gallery/Newline field case, where the resolved address came from a
// second interface with no local route — this file resolves the instance's
// host name (DNS_SERVICE_INSTANCE pszHostName, e.g. "Gallery.local") for
// any additional IPv4 addresses, so preferLocalAddress has more than one
// candidate to choose from.
//
// This file carries no build tag: the lookup and cache are plain Go and are
// tested on every platform, even though only the Windows backend
// (discovery_native_windows.go, via deviceFromDNSSD) currently calls into it.
//
// On Windows, net.Resolver.LookupIP does not go through Go's own DNS client:
// runtime/net's conf.go (goosPrefersCgo) reports true for GOOS=="windows",
// so hostLookupOrder falls back to hostLookupCgo; lookup_windows.go's
// (*Resolver).lookupIP then calls syscall.GetAddrInfoW directly for that
// order. Despite the "cgo" name, that path is a plain syscall wrapper on
// Windows (lookup_windows.go: "Note that on Windows the cgo resolver does
// not actually use cgo") and is unaffected by CGO_ENABLED. GetAddrInfoW is
// serviced by the Windows DNS client, the same component that resolves
// mDNS ".local" names on Windows 10 and later (the same service
// discovery_native_windows.go's own comment cites for DNS-SD). Verified
// against this toolchain's own source (go1.27.1, GOROOT/src/net/
// lookup_windows.go and conf.go) — see log.md for the exact excerpts cited.

// hostLookupTimeout bounds a single host-name lookup so a hung or dead mDNS
// responder never stalls discovery by more than this.
const hostLookupTimeout = 2 * time.Second

// hostLookupCacheTTL is how long a lookup result (success or failure) for a
// given host name is reused, so repeated scans (the daemon runs one every
// 5s) don't re-query for a device that already got an answer, or already
// failed to get one, moments ago.
const hostLookupCacheTTL = 60 * time.Second

// lookupHostIPv4 resolves host (an mDNS "<instance>.local" name) for its
// IPv4 addresses. Overridable for tests; defaults to the system resolver
// bounded by hostLookupTimeout.
var lookupHostIPv4 = func(ctx context.Context, host string) ([]net.IP, error) {
	ctx, cancel := context.WithTimeout(ctx, hostLookupTimeout)
	defer cancel()
	return net.DefaultResolver.LookupIP(ctx, "ip4", host)
}

type hostLookupCacheEntry struct {
	addrs []net.IP
	at    time.Time
}

var (
	hostLookupMu    sync.Mutex
	hostLookupCache = map[string]hostLookupCacheEntry{}
)

// lookupAdditionalIPv4 returns host's IPv4 addresses, using a cached result
// when one was obtained within hostLookupCacheTTL. A failed or timed-out
// lookup is cached too (as an empty result), so a host that consistently
// fails to resolve isn't re-queried every scan either. Errors are logged at
// debug level and otherwise swallowed: the caller falls back to whatever
// address it already had.
func lookupAdditionalIPv4(host string) []net.IP {
	if host == "" {
		return nil
	}

	hostLookupMu.Lock()
	if entry, ok := hostLookupCache[host]; ok && time.Since(entry.at) < hostLookupCacheTTL {
		hostLookupMu.Unlock()
		return entry.addrs
	}
	hostLookupMu.Unlock()

	addrs, err := lookupHostIPv4(context.Background(), host)
	if err != nil {
		dbg("[discovery] host name lookup for %q failed: %v", host, err)
		addrs = nil
	}

	hostLookupMu.Lock()
	hostLookupCache[host] = hostLookupCacheEntry{addrs: addrs, at: time.Now()}
	hostLookupMu.Unlock()
	return addrs
}

// resetHostLookupCacheForTest clears the host-name lookup cache. Test-only.
func resetHostLookupCacheForTest() {
	hostLookupMu.Lock()
	hostLookupCache = map[string]hostLookupCacheEntry{}
	hostLookupMu.Unlock()
}

// isAddressLocal reports whether ip is inside one of the host's directly
// attached IPv4 networks (see localIPv4Networks in local_address.go).
func isAddressLocal(ip net.IP) bool {
	for _, n := range localIPv4Networks() {
		if n != nil && n.Contains(ip) {
			return true
		}
	}
	return false
}

// dedupeIPv4 removes duplicate addresses, preserving first-seen order.
func dedupeIPv4(addrs []net.IP) []net.IP {
	seen := make(map[string]bool, len(addrs))
	out := make([]net.IP, 0, len(addrs))
	for _, a := range addrs {
		key := a.String()
		if seen[key] {
			continue
		}
		seen[key] = true
		out = append(out, a)
	}
	return out
}
