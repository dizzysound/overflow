package airplay

import (
	"errors"
	"net"
	"testing"
	"time"
)

// withAACELDEncoderProbe pins whether this build appears able to encode
// AAC-ELD, independent of the fdk_aac build tag used to run the tests.
func withAACELDEncoderProbe(t *testing.T, err error) {
	t.Helper()
	previous := aacELDEncoderProbe
	aacELDEncoderProbe = func() error { return err }
	t.Cleanup(func() { aacELDEncoderProbe = previous })
}

// setupLegacyAACELDOnlyMirror connects to a receiver shaped like the Newline
// Cast panel from the 2026-09-26 site test: AppleTV3,2 / 220.68, no
// supportedFormats, and a legacy audioFormats type-96 mask of AAC-ELD only.
// The fixture rejects any audio SETUP descriptor that is not AAC-ELD, so an
// ALAC descriptor fails SetupMirror.
func setupLegacyAACELDOnlyMirror(t *testing.T, cfg StreamConfig) (*ReceiverServer, *MirrorSession) {
	t.Helper()
	server, client, ctx := newReceiverServerTestPair(t, ReceiverConfig{
		Profile:                  ReceiverProfileAppleTV3,
		LegacyAudioOutputFormats: screenAudioFormatAACELD44100Stereo,
	})
	if client.info.SupportedFormats.ScreenStream != 0 {
		t.Fatalf("fixture advertised supportedFormats.screenStream 0x%x; want legacy audioFormats only",
			uint64(client.info.SupportedFormats.ScreenStream))
	}
	if got := uint64(client.info.legacyScreenAudioFormats()); got != screenAudioFormatAACELD44100Stereo {
		t.Fatalf("legacy type-96 mask = 0x%x, want AAC-ELD only", got)
	}
	if err := client.Pair(ctx, ""); err != nil {
		t.Fatalf("pair: %v", err)
	}
	if err := client.FairPlaySetup(ctx); err != nil {
		t.Fatalf("FairPlay setup: %v", err)
	}
	session, err := client.SetupMirror(ctx, cfg)
	if err != nil {
		t.Fatalf("setup mirror: %v", err)
	}
	return server, session
}

func TestAACELDOnlyReceiverStreamsVideoOnlyWhenEncoderUnavailable(t *testing.T) {
	withAACELDEncoderProbe(t, ErrAACELDUnavailable)
	server, session := setupLegacyAACELDOnlyMirror(t, StreamConfig{})

	if session.HasAudio() {
		t.Fatalf("session has an audio stream (codec %d); want video only", session.AudioCodec())
	}
	if err := session.AudioUnavailable(); !errors.Is(err, ErrReceiverRequiresAACELD) {
		t.Fatalf("AudioUnavailable() = %v, want ErrReceiverRequiresAACELD", err)
	}

	frame := session.streamCipher(receiverTestAVCC([]byte{0x65, 0x80}))
	timestamp, timeline := session.frameTimeNow()
	if err := session.sendFrame(frame, true, timestamp, timeline); err != nil {
		t.Fatalf("send video frame: %v", err)
	}
	deadline := time.Now().Add(time.Second)
	for server.Stats().VideoDecrypted == 0 && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if err := session.Close(); err != nil && !errors.Is(err, net.ErrClosed) {
		t.Fatalf("close: %v", err)
	}
	stats := server.Stats()
	if stats.VideoDecrypted == 0 {
		t.Fatalf("video did not reach the receiver: %+v", stats)
	}
	if stats.AudioPackets != 0 {
		t.Fatalf("receiver got %d audio packets; want none", stats.AudioPackets)
	}
}

func TestAACELDOnlyReceiverKeepsAudioWhenEncoderAvailable(t *testing.T) {
	withAACELDEncoderProbe(t, nil)
	_, session := setupLegacyAACELDOnlyMirror(t, StreamConfig{})
	defer session.Close()
	if !session.HasAudio() || session.AudioCodec() != AudioCodecAACELD {
		t.Fatalf("HasAudio=%t codec=%d; want an AAC-ELD audio stream", session.HasAudio(), session.AudioCodec())
	}
	if err := session.AudioUnavailable(); err != nil {
		t.Fatalf("AudioUnavailable() = %v, want nil", err)
	}
}

func TestAACELDOnlyReceiverNoAudioDoesNotReportUnavailableAudio(t *testing.T) {
	withAACELDEncoderProbe(t, ErrAACELDUnavailable)
	_, session := setupLegacyAACELDOnlyMirror(t, StreamConfig{NoAudio: true})
	defer session.Close()
	if err := session.AudioUnavailable(); err != nil {
		t.Fatalf("AudioUnavailable() with NoAudio = %v, want nil", err)
	}
}
