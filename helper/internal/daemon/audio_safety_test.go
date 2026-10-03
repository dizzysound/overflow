package daemon

import (
	"bytes"
	"log"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

type lockedBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *lockedBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *lockedBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

// TestEmbeddedDaemonStreamsVideoOnlyToAACELDOnlyLegacyReceiver reproduces the
// 2026-09-26 site receiver (AppleTV3,2 / 220.68, legacy audioFormats type-96
// mask AAC-ELD only, no supportedFormats) with a build that cannot encode
// AAC-ELD. The receiver rejects an ALAC descriptor, so reaching StateStreaming
// proves SETUP carried AAC-ELD; zero audio packets proves nothing followed.
func TestEmbeddedDaemonStreamsVideoOnlyToAACELDOnlyLegacyReceiver(t *testing.T) {
	if capture, err := airplay.NewExternalAudioSource().Subscribe(airplay.AudioCodecAACELD); err == nil {
		capture.Stop()
		t.Skip("this build can encode AAC-ELD; the video-only degrade does not apply")
	}
	logs := &lockedBuffer{}
	log.SetOutput(logs)
	t.Cleanup(func() { log.SetOutput(os.Stderr) })

	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileAppleTV3, withLegacyAudioOutputFormats(0x1000000))

	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		st := server.Stats()
		snap := d.Snapshot()
		live := len(snap.Streams) == 1 && snap.Streams[0].State == StateStreaming
		if live && st.VideoDecrypted > 0 {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if st := server.Stats(); st.VideoDecrypted == 0 {
		t.Fatalf("no decrypted video within 20 s: stats=%+v snapshot=%+v logs:\n%s", st, d.Snapshot(), logs.String())
	}
	// Give any audio path time to start: the external source feeds a tone
	// every 10 ms, so a live audio stream would deliver packets well within this.
	time.Sleep(time.Second)
	if st := server.Stats(); st.AudioPackets != 0 {
		t.Fatalf("receiver got %d audio packets; want none (video only)", st.AudioPackets)
	}
	const warning = "[daemon] Loopback TV: receiver only accepts AAC-ELD audio, which this build cannot encode; streaming video without audio"
	if got := strings.Count(logs.String(), warning); got != 1 {
		t.Fatalf("warning logged %d times, want once; logs:\n%s", got, logs.String())
	}
	if strings.Contains(logs.String(), "audio capture started") {
		t.Fatalf("daemon started audio capture for a video-only session; logs:\n%s", logs.String())
	}
}
