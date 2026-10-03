//go:build windows

package airplay

import (
	"context"
	"os"
	"testing"
	"time"
)

// failOrSkipInCI fails locally (the church PC must not hide a broken API) but
// skips on GitHub's windows-latest runner, a server SKU with no receivers
// where the DNS-SD service may be absent.
func failOrSkipInCI(t *testing.T, format string, args ...any) {
	t.Helper()
	if os.Getenv("GITHUB_ACTIONS") == "true" {
		t.Skipf("native DNS-SD failed on the CI runner: "+format, args...)
	}
	t.Fatalf(format, args...)
}

func TestNativeDNSSDBrowsesAirPlay(t *testing.T) {
	if err := nativeDiscoveryAvailable(); err != nil {
		t.Skipf("native DNS-SD unavailable: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
	defer cancel()
	devices, err := discoverNative(ctx)
	if err != nil {
		failOrSkipInCI(t, "discoverNative: %v", err)
	}
	if len(devices) == 0 {
		t.Skip("no AirPlay receivers answered within 6 s")
	}
	for _, d := range devices {
		t.Logf("%q at %s:%d deviceid=%q model=%q srcvers=%q", d.Name, d.IP, d.Port, d.DeviceID, d.Model, d.SourceVersion)
		if d.IP == "" || d.Port == 0 {
			t.Errorf("%q: missing address", d.Name)
		}
		if d.DeviceID == "" {
			t.Errorf("%q: no deviceid in TXT %v", d.Name, d.RawTXT)
		}
	}
}

// TestNativeDNSSDRepeatedScans runs back-to-back scans the way the daemon does
// (one per 5 s), checking that cancellation and callback reuse hold up.
func TestNativeDNSSDRepeatedScans(t *testing.T) {
	if err := nativeDiscoveryAvailable(); err != nil {
		t.Skipf("native DNS-SD unavailable: %v", err)
	}
	for i := 1; i <= 3; i++ {
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		devices, err := discoverNative(ctx)
		cancel()
		if err != nil {
			failOrSkipInCI(t, "scan %d: %v", i, err)
		}
		t.Logf("scan %d: %d devices", i, len(devices))
	}
}
