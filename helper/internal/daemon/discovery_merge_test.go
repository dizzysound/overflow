package daemon

import (
	"net"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

// Regression coverage for the on-site symptom: zeroconf sometimes delivers an
// mDNS entry with an address but no TXT record (RawTXT empty), which blanked
// the stored model/device ID/features/pk for a receiver that had already been
// fully discovered, flapping the devices list and breaking room lookups keyed
// by device ID.

func completeDevice(ip string) airplay.AirPlayDevice {
	return airplay.AirPlayDevice{
		Name:     "Friendship",
		IP:       ip,
		Port:     7000,
		Model:    "AppleTV3,2",
		DeviceID: "02:52:16:0A:AE:57",
		PK:       "abc123",
		Features: 0x527FFFF7,
		RawTXT:   map[string]string{"deviceid": "02:52:16:0A:AE:57", "model": "AppleTV3,2"},
	}
}

func partialDevice(ip string) airplay.AirPlayDevice {
	return airplay.AirPlayDevice{
		IP:   ip,
		Port: 7000,
	}
}

func TestMergeDiscoveredDeviceKeepsCompleteWhenNewIsPartial(t *testing.T) {
	prev := completeDevice("10.0.0.5")
	next := partialDevice("10.0.0.5")

	got := mergeDiscoveredDevice(prev, next)

	if got.Model != prev.Model || got.DeviceID != prev.DeviceID || got.PK != prev.PK || got.Features != prev.Features {
		t.Fatalf("partial record blanked TXT-derived fields: %+v", got)
	}
	if len(got.RawTXT) == 0 {
		t.Fatalf("RawTXT was dropped: %+v", got)
	}
}

func TestMergeDiscoveredDeviceTakesCompleteWhenPrevWasPartial(t *testing.T) {
	prev := partialDevice("10.0.0.5")
	next := completeDevice("10.0.0.5")

	got := mergeDiscoveredDevice(prev, next)

	if got.Model != next.Model || got.DeviceID != next.DeviceID || got.PK != next.PK {
		t.Fatalf("new complete record was not adopted: %+v", got)
	}
}

func TestMergeDiscoveredDeviceTakesNewCompleteRecordOverOldCompleteRecord(t *testing.T) {
	prev := completeDevice("10.0.0.5")
	next := completeDevice("10.0.0.5")
	next.Model = "AppleTV14,1"
	next.DeviceID = "AA:BB:CC:DD:EE:FF"

	got := mergeDiscoveredDevice(prev, next)

	if got.Model != next.Model || got.DeviceID != next.DeviceID {
		t.Fatalf("a newer complete record for the same IP should replace the old one: %+v", got)
	}
}

func TestMergeDiscoveredDeviceUpdatesPortAndNameOnlyWhenNonEmpty(t *testing.T) {
	prev := completeDevice("10.0.0.5")
	next := partialDevice("10.0.0.5")
	next.Port = 7001
	next.Name = "" // empty: keep prev's name

	got := mergeDiscoveredDevice(prev, next)

	if got.Port != 7001 {
		t.Fatalf("non-empty new port was not taken: %+v", got)
	}
	if got.Name != prev.Name {
		t.Fatalf("empty new name should not blank the previous name: %+v", got)
	}
}

// TestMergeDiscoveredDeviceKeepsIPsWhenNewIsPartial covers the same
// TXT-blanking hazard as TestMergeDiscoveredDeviceKeepsCompleteWhenNewIsPartial
// but for the multi-address list: a partial mDNS entry (no TXT) for a device
// already known with multiple addresses must not drop the address list, or a
// later dial would fall back to a single, possibly wrong, address.
func TestMergeDiscoveredDeviceKeepsIPsWhenNewIsPartial(t *testing.T) {
	prev := completeDevice("10.20.0.164")
	prev.IPs = []net.IP{net.ParseIP("192.168.1.168"), net.ParseIP("10.20.0.164")}
	next := partialDevice("10.20.0.164")
	next.IPs = nil

	got := mergeDiscoveredDevice(prev, next)

	if len(got.IPs) != 2 {
		t.Fatalf("IPs dropped by a partial record: %+v", got.IPs)
	}
}

// TestMergeDiscoveredDeviceTakesNewIPsWhenPresent ensures a fresh, complete
// scan's address list replaces the stored one (e.g. the receiver picked up a
// new address), rather than being stuck on whatever was first observed.
func TestMergeDiscoveredDeviceTakesNewIPsWhenPresent(t *testing.T) {
	prev := completeDevice("10.20.0.164")
	prev.IPs = []net.IP{net.ParseIP("10.20.0.164")}
	next := completeDevice("10.20.0.164")
	next.IPs = []net.IP{net.ParseIP("10.20.0.164"), net.ParseIP("10.20.0.200")}

	got := mergeDiscoveredDevice(prev, next)

	if len(got.IPs) != 2 {
		t.Fatalf("new IPs list was not adopted: %+v", got.IPs)
	}
}

// TestBackgroundDiscoverMergeDoesNotFlapOnRepeatedMultiAddressScans is the
// non-flapping guarantee for the merge step itself: feeding the same
// multi-address discovery result through mergeDiscoveredDevice repeatedly
// (standing in for consecutive 5s scans) must keep producing the same IP
// identity, since that string is the daemon's map key for the device.
func TestBackgroundDiscoverMergeDoesNotFlapOnRepeatedMultiAddressScans(t *testing.T) {
	scanResult := completeDevice("10.20.0.164")
	scanResult.IPs = []net.IP{net.ParseIP("192.168.1.168"), net.ParseIP("10.20.0.164")}

	known := scanResult
	for i := 0; i < 5; i++ {
		known = mergeDiscoveredDevice(known, scanResult)
		if known.IP != scanResult.IP {
			t.Fatalf("scan %d: IP identity flapped, got %q want %q", i, known.IP, scanResult.IP)
		}
	}
}

func TestEffectiveDeviceTTLDefaultsWhenZero(t *testing.T) {
	if got := effectiveDeviceTTL(0); got != defaultDeviceTTL {
		t.Fatalf("effectiveDeviceTTL(0) = %v, want default %v", got, defaultDeviceTTL)
	}
	if got := effectiveDeviceTTL(2 * time.Minute); got != 2*time.Minute {
		t.Fatalf("effectiveDeviceTTL(2m) = %v, want 2m", got)
	}
}
