package airplay

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"log"
	"strings"
	"testing"
)

type discoverFunc = func(context.Context) ([]AirPlayDevice, error)

func stubDiscovery(t *testing.T, native, zeroconf discoverFunc) {
	t.Helper()
	oldNative, oldZeroconf := nativeDiscover, zeroconfDiscover
	nativeDiscover, zeroconfDiscover = native, zeroconf
	discoveryMu.Lock()
	nativeEmptyStreak, nativeEmptyNoted = 0, false
	discoveryMu.Unlock()
	t.Cleanup(func() {
		nativeDiscover, zeroconfDiscover = oldNative, oldZeroconf
		_ = SetDiscoveryBackend(DiscoveryAuto)
		discoveryMu.Lock()
		nativeEmptyStreak, nativeEmptyNoted = 0, false
		discoveryMu.Unlock()
	})
}

func found(name string) discoverFunc {
	return func(context.Context) ([]AirPlayDevice, error) { return []AirPlayDevice{{Name: name}}, nil }
}

func empty() discoverFunc {
	return func(context.Context) ([]AirPlayDevice, error) { return nil, nil }
}

func TestDiscoverPrefersNative(t *testing.T) {
	stubDiscovery(t, found("native"), func(context.Context) ([]AirPlayDevice, error) {
		t.Fatal("zeroconf used although native discovery succeeded")
		return nil, nil
	})
	got, err := DiscoverAirPlayDevices(context.Background())
	if err != nil || len(got) != 1 || got[0].Name != "native" {
		t.Fatalf("got %+v, %v", got, err)
	}
}

func TestDiscoverFallsBackToZeroconf(t *testing.T) {
	for _, nativeErr := range []error{
		errNativeDiscoveryUnavailable,
		fmt.Errorf("%w: DnsServiceBrowse not found", errNativeDiscoveryUnavailable),
		errors.New("DnsServiceBrowse(_airplay._tcp.local): Access is denied."),
	} {
		stubDiscovery(t, func(context.Context) ([]AirPlayDevice, error) { return nil, nativeErr }, found("zeroconf"))
		got, err := DiscoverAirPlayDevices(context.Background())
		if err != nil || len(got) != 1 || got[0].Name != "zeroconf" {
			t.Fatalf("native error %v: got %+v, %v", nativeErr, got, err)
		}
	}
}

func TestDiscoverZeroconfBackendSkipsNative(t *testing.T) {
	stubDiscovery(t, func(context.Context) ([]AirPlayDevice, error) {
		t.Fatal("native discovery used with the zeroconf backend")
		return nil, nil
	}, found("zeroconf"))
	if err := SetDiscoveryBackend(DiscoveryZeroconf); err != nil {
		t.Fatal(err)
	}
	got, err := DiscoverAirPlayDevices(context.Background())
	if err != nil || len(got) != 1 || got[0].Name != "zeroconf" {
		t.Fatalf("got %+v, %v", got, err)
	}
}

func TestNativeEmptyScanHintLogsOnceAfterThreshold(t *testing.T) {
	stubDiscovery(t, empty(), func(context.Context) ([]AirPlayDevice, error) {
		t.Fatal("zeroconf used although native discovery succeeded")
		return nil, nil
	})

	var buf bytes.Buffer
	oldOut := log.Writer()
	oldFlags := log.Flags()
	log.SetOutput(&buf)
	log.SetFlags(0)
	t.Cleanup(func() {
		log.SetOutput(oldOut)
		log.SetFlags(oldFlags)
	})

	for i := 0; i < nativeEmptyScanThreshold-1; i++ {
		if _, err := DiscoverAirPlayDevices(context.Background()); err != nil {
			t.Fatal(err)
		}
	}
	if buf.Len() != 0 {
		t.Fatalf("hint logged before the threshold: %q", buf.String())
	}

	if _, err := DiscoverAirPlayDevices(context.Background()); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(buf.String(), "-discovery zeroconf") || !strings.Contains(buf.String(), "-device") {
		t.Fatalf("missing hint after threshold: %q", buf.String())
	}

	buf.Reset()
	if _, err := DiscoverAirPlayDevices(context.Background()); err != nil {
		t.Fatal(err)
	}
	if buf.Len() != 0 {
		t.Fatalf("hint logged a second time: %q", buf.String())
	}
}

func TestNativeEmptyScanHintResetsOnAFind(t *testing.T) {
	// A found scan sits right before the threshold each time, so the streak
	// never reaches it: the hint must never fire.
	calls := 0
	stubDiscovery(t, func(ctx context.Context) ([]AirPlayDevice, error) {
		calls++
		if calls%nativeEmptyScanThreshold == 0 {
			return []AirPlayDevice{{Name: "native"}}, nil
		}
		return nil, nil
	}, func(context.Context) ([]AirPlayDevice, error) {
		t.Fatal("zeroconf used although native discovery succeeded")
		return nil, nil
	})

	var buf bytes.Buffer
	oldOut := log.Writer()
	log.SetOutput(&buf)
	t.Cleanup(func() { log.SetOutput(oldOut) })

	for i := 0; i < nativeEmptyScanThreshold*3; i++ {
		if _, err := DiscoverAirPlayDevices(context.Background()); err != nil {
			t.Fatal(err)
		}
	}
	if buf.Len() != 0 {
		t.Fatalf("hint logged although a scan found a receiver: %q", buf.String())
	}
}

func TestSetDiscoveryBackend(t *testing.T) {
	t.Cleanup(func() { _ = SetDiscoveryBackend(DiscoveryAuto) })
	for _, ok := range []string{"", "auto", "zeroconf"} {
		if err := SetDiscoveryBackend(ok); err != nil {
			t.Errorf("SetDiscoveryBackend(%q) = %v", ok, err)
		}
	}
	if err := SetDiscoveryBackend("bonjour"); err == nil {
		t.Error("SetDiscoveryBackend(bonjour) = nil, want an error")
	}
}
