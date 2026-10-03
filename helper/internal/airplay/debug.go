package airplay

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"log"
	"regexp"
	"sync/atomic"
)

var debugMode atomic.Bool

// SetDebugMode controls verbose AirPlay logging. It is safe to call while
// capture or receiver workers are active.
func SetDebugMode(enabled bool) {
	debugMode.Store(enabled)
}

// DebugMode reports whether verbose AirPlay logging is enabled.
func DebugMode() bool {
	return debugMode.Load()
}

// dbg logs a message only when debug mode is enabled.
//
// Never log key material (session keys, IVs, shared secrets, derived HKDF
// output, or FairPlay/HAP handshake payloads) directly through dbg, even
// with -debug. Route any such byte slice through keyFingerprint first: the
// helper's "Save diagnostic log..." feature captures this output verbatim,
// so a raw secret logged here can end up in a file an operator shares.
// Public keys are not secret and may be logged as-is; say so at the call
// site with a comment.
//
// dbg also elides any long hex run (32+ hex characters, or 16+ space-separated
// bytes) from the formatted line: packet, frame and handshake dumps are noise
// in an operator's report and some are plaintext or secrets. Short values such
// as fingerprints, public-key prefixes and header bytes pass through.
func dbg(format string, args ...interface{}) {
	if DebugMode() {
		log.Print(elideLongHex(fmt.Sprintf(format, args...)))
	}
}

var (
	longHexDigits = regexp.MustCompile(`[0-9a-fA-F]{32,}`)
	longHexBytes  = regexp.MustCompile(`(?:[0-9a-fA-F]{2} ){15,}[0-9a-fA-F]{2}`)
)

func elideLongHex(line string) string {
	line = longHexBytes.ReplaceAllStringFunc(line, func(m string) string {
		return fmt.Sprintf("<%d bytes elided>", (len(m)+1)/3)
	})
	return longHexDigits.ReplaceAllStringFunc(line, func(m string) string {
		return fmt.Sprintf("<%d hex chars elided>", len(m))
	})
}

// keyFingerprint returns a non-reversible fingerprint of sensitive byte
// material — keys, IVs, shared secrets, and other derived key material —
// suitable for debug logs. It reveals only the length and the first 4
// bytes of the SHA-256 digest, never the underlying bytes, so it is safe
// to include in helper stderr that may be captured into a diagnostic log.
func keyFingerprint(b []byte) string {
	sum := sha256.Sum256(b)
	return fmt.Sprintf("fp=%s (%d bytes)", hex.EncodeToString(sum[:4]), len(b))
}
