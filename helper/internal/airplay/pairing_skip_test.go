package airplay

import (
	"context"
	"testing"
)

// Feature words observed on 2026-09-28.
const (
	featuresNewlineCast   uint64 = 0xE527FFFF7        // Newline Cast (DisplayNote) on the Gallery panel
	featuresUxPlayDesktop uint64 = 0x527FFEE6         // UxPlay default: no legacy pairing
	featuresUxPlayAndroid uint64 = 0x4005A7FFEF7      // jqssun Android receiver: legacy pairing (bit 27)
	featuresRoku          uint64 = 255525703439583952 // Hisense Roku: transient and system pairing
	featuresAppleTVHD     uint64 = 4330034975345729493
)

func TestReceiverExpectsNoPairing(t *testing.T) {
	cases := []struct {
		name     string
		features uint64
		want     bool
	}{
		{"Newline Cast", featuresNewlineCast, true},
		{"UxPlay desktop", featuresUxPlayDesktop, true},
		{"UxPlay Android (feature 27)", featuresUxPlayAndroid, false},
		{"Roku (transient pairing)", featuresRoku, false},
		{"Apple TV HD", featuresAppleTVHD, false},
	}
	for _, tc := range cases {
		info := &ReceiverInfo{Features: tc.features}
		if got := info.expectsNoPairing(); got != tc.want {
			t.Errorf("%s (%#x): expectsNoPairing = %v, want %v", tc.name, tc.features, got, tc.want)
		}
	}
}

// Apple senders do no pair-setup or pair-verify with a receiver that
// advertises neither legacy (27) nor transient/system (48/43) pairing. Newline
// Cast decrypts but never renders mirrored video after a pair-verify it did
// not ask for; skipping pairing made the Gallery panel show video.
func TestPairSkipsReceiverThatAdvertisesNoPairing(t *testing.T) {
	c := &AirPlayClient{info: &ReceiverInfo{Name: "Gallery", Features: featuresNewlineCast}}
	// conn is nil: any pairing request would fail, so success proves nothing was sent.
	if err := c.Pair(context.Background(), ""); err != nil {
		t.Fatalf("Pair: %v, want nil with no pairing requests", err)
	}
	if c.PairKeys != nil {
		t.Fatalf("PairKeys = %+v, want nil (no pair-verify secret)", c.PairKeys)
	}
}
