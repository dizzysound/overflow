package airplay

import (
	"bytes"
	"log"
	"regexp"
	"strings"
	"testing"
)

// Long hex runs in -debug output are packet, frame and handshake dumps. They
// are not useful to an operator, bloat the diagnostic report, and some are
// secrets or plaintext. Only short values (fingerprints, public-key prefixes,
// header bytes, type codes) may appear.
var longHexRun = regexp.MustCompile(`[0-9a-fA-F]{32,}|(?:[0-9a-fA-F]{2} ){15,}[0-9a-fA-F]{2}`)

func captureDebugLog(t *testing.T) *bytes.Buffer {
	t.Helper()
	var buf bytes.Buffer
	prevOutput, prevFlags, prevDebug := log.Writer(), log.Flags(), DebugMode()
	log.SetOutput(&buf)
	log.SetFlags(0)
	SetDebugMode(true)
	t.Cleanup(func() {
		log.SetOutput(prevOutput)
		log.SetFlags(prevFlags)
		SetDebugMode(prevDebug)
	})
	return &buf
}

func TestDbgElidesLongHexDumps(t *testing.T) {
	buf := captureDebugLog(t)
	payload := bytes.Repeat([]byte{0xAB, 0xCD}, 20) // 40 bytes
	dbg("[SEND] codec payload: %02x", payload)
	dbg("[CAPTURE] read %d bytes start=% x", 40, payload[:20])
	dbg("[PAIR] raw pair-setup OK, server Ed25519 pub: %02x", payload[:8])
	dbg("[SETUP] shared secret: %s", keyFingerprint(payload))
	out := buf.String()
	if m := longHexRun.FindString(out); m != "" {
		t.Fatalf("debug output still contains a long hex run %q:\n%s", m, out)
	}
	for _, want := range []string{"<80 hex chars elided>", "abcdabcdabcdabcd", "fp="} {
		if !strings.Contains(out, want) {
			t.Fatalf("debug output missing %q:\n%s", want, out)
		}
	}
}

// A whole mirror session with -debug, on a legacy FairPlay receiver and a
// modern HAP one, must not put a long hex run in the log.
func TestDebugSessionLogHasNoLongHexRuns(t *testing.T) {
	for _, profile := range []ReceiverProfile{ReceiverProfileUxPlayAndroid, ReceiverProfileModern} {
		t.Run(string(profile), func(t *testing.T) {
			buf := captureDebugLog(t)
			_, client, ctx := newReceiverServerTestPair(t, ReceiverConfig{Profile: profile, Auth: ReceiverAuthNone})
			if err := client.Pair(ctx, ""); err != nil {
				t.Fatalf("pair: %v", err)
			}
			if client.info.SupportsFairPlaySAP() {
				if err := client.FairPlaySetup(ctx); err != nil {
					t.Fatalf("FairPlay setup: %v", err)
				}
			}
			session, err := client.SetupMirror(ctx, StreamConfig{})
			if err != nil {
				t.Fatalf("setup mirror: %v", err)
			}
			_ = session.Close()
			if m := longHexRun.FindString(buf.String()); m != "" {
				t.Fatalf("session debug log contains a long hex run (%d chars)", len(m))
			}
		})
	}
}
