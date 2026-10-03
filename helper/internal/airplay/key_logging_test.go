package airplay

import (
	"bytes"
	"encoding/base64"
	"encoding/hex"
	"log"
	"strings"
	"testing"
)

// TestDebugLoggingNeverLeaksKeyMaterial runs a full loopback mirror session
// against the in-process test receiver with verbose (-debug) logging
// enabled, and asserts that none of the session's negotiated secret bytes
// (FairPlay keys/IV, pair-verify shared secret, and HAP control channel
// keys) appear anywhere in the captured log output, in hex or base64. It
// also asserts that the fingerprint markers this security fix introduced
// do appear, so the test would fail if fingerprinting were silently
// removed rather than just the raw secrets.
//
// This guards against the helper's "Save diagnostic log..." feature (which
// captures helper stderr verbatim) ever shipping session key material to an
// operator-shared file.
func TestDebugLoggingNeverLeaksKeyMaterial(t *testing.T) {
	var logBuf bytes.Buffer
	prevOutput := log.Writer()
	prevFlags := log.Flags()
	log.SetOutput(&logBuf)
	log.SetFlags(0)
	t.Cleanup(func() {
		log.SetOutput(prevOutput)
		log.SetFlags(prevFlags)
	})

	prevDebug := DebugMode()
	SetDebugMode(true)
	t.Cleanup(func() { SetDebugMode(prevDebug) })

	server, client, ctx := newReceiverServerTestPair(t, ReceiverConfig{
		Profile: ReceiverProfileModern,
		Auth:    ReceiverAuthNone,
	})
	_ = server

	if err := client.Pair(ctx, ""); err != nil {
		t.Fatalf("pair: %v", err)
	}
	if client.info.SupportsFairPlaySAP() {
		if err := client.FairPlaySetup(ctx); err != nil {
			t.Fatalf("FairPlay setup: %v", err)
		}
	}

	session, err := client.SetupMirror(ctx, StreamConfig{NoAudio: false})
	if err != nil {
		t.Fatalf("setup mirror: %v", err)
	}
	if err := session.Close(); err != nil {
		t.Logf("close mirror session: %v", err)
	}

	logOutput := logBuf.String()

	// Collect every actual secret negotiated during this session. These are
	// the exact byte slices that the call sites fixed by this change used to
	// print via %x/hex.EncodeToString.
	secrets := map[string][]byte{
		"fpAesKey":    client.fpAesKey,
		"fpKey":       client.fpKey,
		"fpIV":        client.fpIV,
		"encWriteKey": client.encWriteKey,
		"encReadKey":  client.encReadKey,
	}
	if client.PairKeys != nil {
		secrets["PairKeys.SharedSecret"] = client.PairKeys.SharedSecret
	}

	found := false
	for name, secret := range secrets {
		if len(secret) < 8 {
			// Too short to be a meaningful leak signal; skip rather than risk
			// a false positive against short incidental substrings.
			continue
		}
		hexLower := hex.EncodeToString(secret)
		hexUpper := strings.ToUpper(hexLower)
		b64 := base64.StdEncoding.EncodeToString(secret)

		if strings.Contains(logOutput, hexLower) {
			t.Errorf("log output contains %s in lowercase hex", name)
			found = true
		}
		if strings.Contains(logOutput, hexUpper) {
			t.Errorf("log output contains %s in uppercase hex", name)
			found = true
		}
		if strings.Contains(logOutput, b64) {
			t.Errorf("log output contains %s in base64", name)
			found = true
		}
		// Also check any 8-byte-or-longer prefix/window of the secret in hex,
		// in case only part of the key was ever printed (as several of the
		// fixed call sites did, e.g. "key[:8]" or "key[:16]").
		for windowLen := 8; windowLen <= len(secret); windowLen++ {
			window := secret[:windowLen]
			if strings.Contains(logOutput, hex.EncodeToString(window)) {
				t.Errorf("log output contains an %d-byte prefix of %s in hex", windowLen, name)
				found = true
				break
			}
		}
	}
	if found {
		t.Logf("captured log output:\n%s", logOutput)
	}

	// The fix routes secret logging through keyFingerprint, which always
	// emits "fp=<hex>(...bytes)". Confirm those markers are present so this
	// test would fail loudly if fingerprinting itself were removed instead
	// of just the raw secrets.
	if !strings.Contains(logOutput, "fp=") {
		t.Error("expected fingerprint markers (\"fp=\") in debug log output, found none")
	}
	if !strings.Contains(logOutput, "[FP] fpAesKey (raw): fp=") {
		t.Error("expected fingerprinted fpAesKey line, got none")
	}
	if !strings.Contains(logOutput, "[FP] fpKey (hashed): fp=") {
		t.Error("expected fingerprinted fpKey line, got none")
	}
	if !strings.Contains(logOutput, "[PAIR-VERIFY] shared secret: fp=") {
		t.Error("expected fingerprinted pair-verify shared secret line, got none")
	}
}
