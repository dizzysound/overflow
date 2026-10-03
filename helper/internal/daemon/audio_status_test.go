package daemon

import (
	"strings"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

func waitAudioStatus(t *testing.T, d *Daemon, want string) StreamInfo {
	t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		snap := d.Snapshot()
		if len(snap.Streams) == 1 && snap.Streams[0].State == StateStreaming && snap.Streams[0].Audio == want {
			return snap.Streams[0]
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("stream audio never became %q: snapshot=%+v", want, d.Snapshot())
	return StreamInfo{}
}

func TestSnapshotReportsAudioOn(t *testing.T) {
	_, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	info := waitAudioStatus(t, d, AudioOn)
	if info.AudioReason != "" {
		t.Fatalf("AudioReason = %q, want empty while audio is on", info.AudioReason)
	}
}

func TestSnapshotReportsAudioOffForAACELDOnlyReceiver(t *testing.T) {
	if capture, err := airplay.NewExternalAudioSource().Subscribe(airplay.AudioCodecAACELD); err == nil {
		capture.Stop()
		t.Skip("this build can encode AAC-ELD; the video-only degrade does not apply")
	}
	_, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileAppleTV3, withLegacyAudioOutputFormats(0x1000000))
	info := waitAudioStatus(t, d, AudioOff)
	if !strings.Contains(info.AudioReason, "AAC-ELD") {
		t.Fatalf("AudioReason = %q, want the AAC-ELD explanation", info.AudioReason)
	}
}
