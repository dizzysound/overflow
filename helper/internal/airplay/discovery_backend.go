package airplay

import (
	"context"
	"errors"
	"fmt"
	"log"
	"sync"
)

// Discovery backends for SetDiscoveryBackend.
const (
	// DiscoveryAuto uses the operating system's DNS-SD service where there is a
	// native backend (Windows 10 1709 and later, dnsapi.dll) and the built-in
	// mDNS listener elsewhere, or when the native call fails.
	DiscoveryAuto = "auto"
	// DiscoveryZeroconf always uses the built-in mDNS listener.
	DiscoveryZeroconf = "zeroconf"
)

// errNativeDiscoveryUnavailable is returned unwrapped by platforms that have
// no native backend (expected, never logged) and wrapped when the Windows API
// is missing (logged once).
var errNativeDiscoveryUnavailable = errors.New("native DNS-SD discovery is not available")

// nativeEmptyScanThreshold is how many consecutive native scans that succeed
// but find no receivers trigger the one-time stderr hint below. A PC where
// the Windows DNS client's mDNS is disabled by policy has native discovery
// succeed forever with zero devices, silently; the built-in zeroconf listener
// or a manual address are the way out, and nothing else tells the operator so.
const nativeEmptyScanThreshold = 3

var (
	discoveryMu        sync.Mutex
	discoveryBackend   = DiscoveryAuto
	nativeFailureNoted bool
	nativeEmptyStreak  int
	nativeEmptyNoted   bool

	// Indirections for tests.
	nativeDiscover   = discoverNative
	zeroconfDiscover = discoverZeroconf
)

// SetDiscoveryBackend selects DiscoveryAuto ("" also means auto) or DiscoveryZeroconf.
func SetDiscoveryBackend(name string) error {
	switch name {
	case "", DiscoveryAuto:
		name = DiscoveryAuto
	case DiscoveryZeroconf:
	default:
		return fmt.Errorf("discovery backend %q: want auto or zeroconf", name)
	}
	discoveryMu.Lock()
	discoveryBackend = name
	discoveryMu.Unlock()
	return nil
}

// DiscoverAirPlayDevices browses the local network for AirPlay receivers
// until ctx ends. Native backends report failures before they start browsing,
// so a fallback to zeroconf still has the whole scan window.
func DiscoverAirPlayDevices(ctx context.Context) ([]AirPlayDevice, error) {
	discoveryMu.Lock()
	backend := discoveryBackend
	discoveryMu.Unlock()
	if backend == DiscoveryAuto {
		devices, err := nativeDiscover(ctx)
		if err == nil {
			noteNativeDiscoveryEmpty(len(devices))
			return devices, nil
		}
		noteNativeDiscoveryFailure(err)
	}
	return zeroconfDiscover(ctx)
}

// noteNativeDiscoveryEmpty tracks consecutive native scans that succeeded but
// found no receivers, and logs a one-time stderr hint once that streak
// crosses nativeEmptyScanThreshold. A scan that finds anything resets it.
func noteNativeDiscoveryEmpty(found int) {
	discoveryMu.Lock()
	defer discoveryMu.Unlock()
	if found > 0 {
		nativeEmptyStreak = 0
		return
	}
	nativeEmptyStreak++
	if nativeEmptyStreak < nativeEmptyScanThreshold || nativeEmptyNoted {
		return
	}
	nativeEmptyNoted = true
	log.Printf("[discovery] native DNS-SD has found no receivers in %d consecutive scans; "+
		"try -discovery zeroconf or -device", nativeEmptyStreak)
}

func noteNativeDiscoveryFailure(err error) {
	if err == errNativeDiscoveryUnavailable {
		return // this platform has no native backend
	}
	discoveryMu.Lock()
	first := !nativeFailureNoted
	nativeFailureNoted = true
	discoveryMu.Unlock()
	if first {
		log.Printf("[discovery] native DNS-SD failed (%v); using the built-in mDNS listener", err)
	} else {
		dbg("[discovery] native DNS-SD failed again: %v", err)
	}
}
