package daemon

import (
	"testing"

	"doubletake/internal/airplay"
)

// withReceiverScreenAudio makes the test receiver require codec in SETUP
// whatever its /info advertises.
func withReceiverScreenAudio(codec airplay.AudioCodec) embeddedDaemonOption {
	return func(s *embeddedSetup) { s.receiver.ScreenAudioCodec = codec }
}

func TestConnectAudioFormatReachesSetup(t *testing.T) {
	// The receiver advertises AAC-ELD only (the automatic choice) but requires
	// ALAC, so only a forced ct=2 descriptor reaches streaming. Forcing ALAC
	// needs no AAC-ELD encoder in the test environment.
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern,
		withLegacyAudioOutputFormats(0x01000000), withReceiverScreenAudio(airplay.AudioCodecALAC))
	resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, AudioFormat: "alac"})
	if !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
}

func TestConnectRejectsUnknownAudioFormat(t *testing.T) {
	_, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, AudioFormat: "opus"})
	if resp.OK {
		t.Fatalf("connect with audio_format opus: %+v, want an error", resp)
	}
}
