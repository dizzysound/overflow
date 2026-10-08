package airplay

import (
	"bytes"
	"crypto/sha512"
	"testing"
)

func TestSelectLegacyKey(t *testing.T) {
	negotiated := bytes.Repeat([]byte{0xAA}, 16)
	raw := bytes.Repeat([]byte{0xBB}, 16)
	secret := bytes.Repeat([]byte{0xCC}, 32)
	if got := selectLegacyKey(LegacyKeyAuto, negotiated, raw, secret); !bytes.Equal(got, negotiated) {
		t.Fatalf("auto: got %x, want the negotiated key", got)
	}
	if got := selectLegacyKey(LegacyKeyRaw, negotiated, raw, secret); !bytes.Equal(got, raw) {
		t.Fatalf("raw: got %x, want raw", got)
	}
	if got := selectLegacyKey(LegacyKeyRaw, negotiated, nil, secret); !bytes.Equal(got, negotiated) {
		t.Fatalf("raw requested but unavailable: got %x, want negotiated fallback", got)
	}
}

// A UxPlay-derived receiver hashes the FairPlay key with the pair-verify
// secret whenever pair-verify completed, even when it advertises feature 27
// (the jqssun Android app on the Gallery panel does). The helper's auto
// rule sends the raw key to feature-27 receivers, so auto's key is raw here;
// mixed must still produce SHA-512(raw || secret)[:16].
func TestSelectLegacyKeyMixedHashesRawKeyWithPairVerifySecret(t *testing.T) {
	raw := bytes.Repeat([]byte{0xBB}, 16)
	secret := bytes.Repeat([]byte{0xCC}, 32)
	sum := sha512.Sum512(append(append([]byte{}, raw...), secret...))
	want := sum[:16]

	negotiatedRaw := raw // auto on a feature-27 receiver
	if got := selectLegacyKey(LegacyKeyMixed, negotiatedRaw, raw, secret); !bytes.Equal(got, want) {
		t.Fatalf("mixed: got %x, want SHA-512(raw||secret)[:16] %x", got, want)
	}
	if got := selectLegacyKey(LegacyKeyMixed, negotiatedRaw, raw, nil); !bytes.Equal(got, negotiatedRaw) {
		t.Fatalf("mixed without a pair-verify secret: got %x, want negotiated fallback", got)
	}
}

func TestParseLegacyKeyMode(t *testing.T) {
	for in, want := range map[string]LegacyKeyMode{"auto": LegacyKeyAuto, "raw": LegacyKeyRaw, "mixed": LegacyKeyMixed} {
		got, err := ParseLegacyKeyMode(in)
		if err != nil || got != want {
			t.Fatalf("ParseLegacyKeyMode(%q) = %v, %v; want %v", in, got, err, want)
		}
	}
	if _, err := ParseLegacyKeyMode("hashed"); err == nil {
		t.Fatal("ParseLegacyKeyMode(\"hashed\") succeeded, want an error")
	}
}
