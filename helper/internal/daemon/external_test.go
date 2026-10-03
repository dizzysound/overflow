package daemon

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"io"
	"log"
	"net"
	"path/filepath"
	"runtime/pprof"
	"testing"
	"time"

	"doubletake/internal/airplay"
	"doubletake/internal/testmedia"
)

// goroutineDump returns every goroutine's stack, grouped by identical stacks,
// so an intermittent 20 s timeout in these loopback tests shows where the
// session was blocked.
func goroutineDump() string {
	var b bytes.Buffer
	b.WriteString("goroutines:\n")
	_ = pprof.Lookup("goroutine").WriteTo(&b, 1)
	return b.String()
}

// feedTestMedia paces the test clip at 30 fps and the tone by elapsed time
// (testmedia.ToneClock) until ctx ends. A nonzero audioStall holds the tone back
// for that long every 2 s and then delivers the backlog with its real capture
// times, as an OBS audio thread that stalls does.
func feedTestMedia(ctx context.Context, video *airplay.ExternalVideoSource, audio *airplay.ExternalAudioSource, audioStall time.Duration) {
	aus := testmedia.VideoAUs()
	start := time.Now()
	tone := testmedia.NewToneClock(start)
	vt := time.NewTicker(time.Second / testmedia.ClipFPS)
	at := time.NewTicker(10 * time.Millisecond)
	defer vt.Stop()
	defer at.Stop()
	for i := 0; ; {
		select {
		case <-ctx.Done():
			return
		case now := <-vt.C:
			video.Push(airplay.VideoAccessUnit{AnnexB: aus[i%len(aus)], PTS: now})
			i++
		case now := <-at.C:
			if audioStall > 0 && now.Sub(start)%(2*time.Second) < audioStall {
				continue // stalled: the blocks stay owed
			}
			for _, b := range tone.Due(now) {
				audio.Push(start.Add(b.Offset), b.PCM)
			}
		}
	}
}

// setupEmbeddedDaemon starts a doubletake receiver on the given profile, feeds
// it external H.264/PCM test media through an embedded daemon, and connects
// the daemon to the receiver. The caller polls server.Stats() / d.Snapshot()
// for its own success condition; cancel() and cleanup happen automatically
// via t.Cleanup.
// embeddedSetup is what an embeddedDaemonOption may change: the daemon and
// receiver configs, and the test media feed.
type embeddedSetup struct {
	cfg        *Config
	receiver   *airplay.ReceiverConfig
	audioStall time.Duration // see feedTestMedia
}

// embeddedDaemonOption tweaks the setup used by setupEmbeddedDaemon, without
// changing its signature for the existing callers.
type embeddedDaemonOption func(*embeddedSetup)

// withForceNTP overrides the negotiated timing protocol to NTP, as -timing ntp does.
func withForceNTP() embeddedDaemonOption {
	return func(s *embeddedSetup) { s.cfg.ForceNTP = true }
}

// withKeyframeRequests records the daemon's keyframe requests.
func withKeyframeRequests(fn func(deviceID, reason string)) embeddedDaemonOption {
	return func(s *embeddedSetup) {
		if s.cfg.External == nil {
			s.cfg.External = &ExternalMedia{}
		}
		s.cfg.External.RequestKeyframe = fn
	}
}

// withAudioDropped records audio frames the daemon drops as too late.
func withAudioDropped(fn func(deviceID string)) embeddedDaemonOption {
	return func(s *embeddedSetup) {
		if s.cfg.External == nil {
			s.cfg.External = &ExternalMedia{}
		}
		s.cfg.External.AudioDropped = fn
	}
}

// withFrameDelivered records each delivered video frame's lead state and age.
func withFrameDelivered(fn func(deviceID string, lead airplay.LeadState, age time.Duration)) embeddedDaemonOption {
	return func(s *embeddedSetup) {
		if s.cfg.External == nil {
			s.cfg.External = &ExternalMedia{}
		}
		s.cfg.External.FrameDelivered = fn
	}
}

// withAudioStall makes the test feed stall its audio (see feedTestMedia).
func withAudioStall(d time.Duration) embeddedDaemonOption {
	return func(s *embeddedSetup) { s.audioStall = d }
}

// withLegacyAudioOutputFormats makes the receiver advertise screen audio only
// through the legacy /info audioFormats array, with this type-96 mask.
func withLegacyAudioOutputFormats(mask uint64) embeddedDaemonOption {
	return func(s *embeddedSetup) { s.receiver.LegacyAudioOutputFormats = mask }
}

func startEmbeddedDaemon(t *testing.T, profile airplay.ReceiverProfile, opts ...embeddedDaemonOption) (server *airplay.ReceiverServer, d *Daemon, addr *net.TCPAddr) {
	t.Helper()
	ctx, cancel := context.WithCancel(context.Background())

	receiverCfg := airplay.ReceiverConfig{
		ListenAddress: "127.0.0.1:0",
		Profile:       profile,
		Auth:          airplay.ReceiverAuthNone,
		Name:          "Loopback TV",
		Logger:        log.New(io.Discard, "", 0),
	}
	cfg := Config{
		CredBackend: "file",
		CredFile:    filepath.Join(t.TempDir(), "credentials.json"),
	}
	setup := embeddedSetup{cfg: &cfg, receiver: &receiverCfg}
	for _, opt := range opts {
		opt(&setup)
	}
	server, err := airplay.NewReceiverServer(receiverCfg)
	if err != nil {
		t.Fatal(err)
	}
	go server.Serve(ctx)

	video := airplay.NewExternalVideoSource(90)
	audio := airplay.NewExternalAudioSource()
	broadcast := airplay.NewBroadcastCaptureWithFrameRate(video.Capture(), 30)
	go broadcast.Run()
	go feedTestMedia(ctx, video, audio, setup.audioStall)

	ext := &ExternalMedia{Video: broadcast, Audio: audio}
	if cfg.External != nil {
		ext.RequestKeyframe = cfg.External.RequestKeyframe
		ext.FrameDelivered = cfg.External.FrameDelivered
		ext.AudioDropped = cfg.External.AudioDropped
	}
	cfg.External = ext

	d, err = New(cfg)
	if err != nil {
		t.Fatal(err)
	}
	go d.RunEmbedded(ctx)

	t.Cleanup(func() {
		cancel()
		d.Shutdown()
		video.Close()
		audio.Close()
		server.Close()
	})
	return server, d, server.Addr().(*net.TCPAddr)
}

// setupEmbeddedDaemon is startEmbeddedDaemon plus a plain connect request.
func setupEmbeddedDaemon(t *testing.T, profile airplay.ReceiverProfile, opts ...embeddedDaemonOption) (server *airplay.ReceiverServer, d *Daemon) {
	t.Helper()
	server, d, addr := startEmbeddedDaemon(t, profile, opts...)
	if resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port}); !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	return server, d
}

// TestEmbeddedDaemonStreamsExternalMedia runs the embedded daemon against the
// modern HAP/PTP receiver profile. The in-process receiver only validates
// video decryption for the legacy AppleTV3/UxPlay profiles (see
// TestEmbeddedDaemonVideoDecryptsOnLegacyReceiver), so against
// ReceiverProfileModern this test asserts flow — packets, dimensions, and
// audio arriving with no crypto errors — rather than decrypted content.
func TestEmbeddedDaemonStreamsExternalMedia(t *testing.T) {
	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileModern)

	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		st := server.Stats()
		snap := d.Snapshot()
		live := len(snap.Streams) == 1 && snap.Streams[0].State == StateStreaming
		if st.VideoCryptoErrors != 0 {
			t.Fatalf("video crypto errors: %d", st.VideoCryptoErrors)
		}
		if live && st.VideoPackets > 0 && st.VideoWidth == testmedia.ClipWidth && st.VideoHeight == testmedia.ClipHeight && st.AudioPackets > 0 {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("no media within 20 s: stats=%+v snapshot=%+v\n%s", server.Stats(), d.Snapshot(), goroutineDump())
}

// TestEmbeddedDaemonDefaultNegotiatesPTPNoNTPProbes establishes the baseline
// for the -timing ntp override: ReceiverProfileModern negotiates PTP by
// default (see receiverProfile in receiver_server.go), so an ordinary run
// against it must show no NTP timing probes.
//
// A true end-to-end ForceNTP counterpart against this same profile is not
// possible with the in-process test receiver: it hard-enforces the
// timingProtocol each profile advertised during SETUP negotiation and
// rejects a mismatch (observed: "mirror setup failed: ... HTTP 400 (body:
// timingProtocol is \"NTP\", want \"PTP\")" against both ReceiverProfileModern
// and ReceiverProfileLG, the only fake profiles that negotiate PTP). See
// TestMirrorStreamConfigForceNTP below for the plumbing assertion instead.
func TestEmbeddedDaemonDefaultNegotiatesPTPNoNTPProbes(t *testing.T) {
	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		st := server.Stats()
		snap := d.Snapshot()
		live := len(snap.Streams) == 1 && snap.Streams[0].State == StateStreaming
		if live && st.VideoPackets > 0 && st.AudioPackets > 0 {
			// Give any (unexpected) NTP probing time to show up before asserting its absence.
			time.Sleep(500 * time.Millisecond)
			if st := server.Stats(); st.TimingProbes != 0 {
				t.Fatalf("default Modern run: TimingProbes = %d, want 0 (PTP expected)", st.TimingProbes)
			}
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("no media within 20 s: stats=%+v snapshot=%+v\n%s", server.Stats(), d.Snapshot(), goroutineDump())
}

// TestEmbeddedDaemonForceNTPDoesNotBreakAnNTPReceiver confirms only that
// setting ForceNTP does not break an ordinary run against a receiver that
// already negotiates NTP by default (ReceiverProfileRoku): media still flows
// and NTP timing probes still appear. Because Roku is NTP by default, this
// run would pass identically with ForceNTP off, so it is NOT evidence that
// the override changed anything — it only exercises the withForceNTP
// setupEmbeddedDaemon option end to end without regressing a real receiver.
// The actual proof that -timing ntp overrides a PTP negotiation is
// TestResolveTimingProtocolForcesNTPOverPTP (internal/airplay/mirror_test.go)
// together with TestMirrorStreamConfigForceNTP below.
func TestEmbeddedDaemonForceNTPDoesNotBreakAnNTPReceiver(t *testing.T) {
	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileRoku, withForceNTP())
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		st := server.Stats()
		snap := d.Snapshot()
		live := len(snap.Streams) == 1 && snap.Streams[0].State == StateStreaming
		if live && st.VideoPackets > 0 && st.AudioPackets > 0 && st.TimingProbes > 0 {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("no media/timing within 20 s: stats=%+v snapshot=%+v\n%s", server.Stats(), d.Snapshot(), goroutineDump())
}

// TestMirrorStreamConfigForceNTP proves the -timing ntp plumbing end to end
// from daemon.Config to the airplay.StreamConfig field the mirror session
// reads, without relying on the in-process test receiver's SETUP negotiation
// (see TestEmbeddedDaemonDefaultNegotiatesPTPNoNTPProbes for why that path
// cannot exercise ForceNTP against a PTP-negotiating profile).
func TestMirrorStreamConfigForceNTP(t *testing.T) {
	d, err := New(Config{
		CredBackend: "file",
		CredFile:    filepath.Join(t.TempDir(), "credentials.json"),
		ForceNTP:    true,
	})
	if err != nil {
		t.Fatal(err)
	}
	if got := d.mirrorStreamConfig(); !got.ForceNTPTiming {
		t.Fatalf("mirrorStreamConfig().ForceNTPTiming = %v, want true when Config.ForceNTP is set", got.ForceNTPTiming)
	}

	dOff, err := New(Config{
		CredBackend: "file",
		CredFile:    filepath.Join(t.TempDir(), "credentials.json"),
	})
	if err != nil {
		t.Fatal(err)
	}
	if got := dOff.mirrorStreamConfig(); got.ForceNTPTiming {
		t.Fatalf("mirrorStreamConfig().ForceNTPTiming = %v, want false by default", got.ForceNTPTiming)
	}
}

// TestEmbeddedDaemonVideoDecryptsOnLegacyReceiver runs the same embedded
// daemon setup against the legacy AppleTV3 profile, which does validate
// decrypted video (see receiver_media.go's drainVideo), giving full
// end-to-end coverage of the external media path including decryption.
func TestEmbeddedDaemonVideoDecryptsOnLegacyReceiver(t *testing.T) {
	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileAppleTV3)

	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		st := server.Stats()
		snap := d.Snapshot()
		live := len(snap.Streams) == 1 && snap.Streams[0].State == StateStreaming
		if st.VideoCryptoErrors != 0 {
			t.Fatalf("video crypto errors: %d", st.VideoCryptoErrors)
		}
		if live && st.VideoDecrypted > 0 && st.AudioPackets > 0 {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("no decrypted media within 20 s: stats=%+v snapshot=%+v\n%s", server.Stats(), d.Snapshot(), goroutineDump())
}

func TestForgetClearsPairing(t *testing.T) {
	cs, err := airplay.NewCredentialStore(filepath.Join(t.TempDir(), "c.json"))
	if err != nil {
		t.Fatal(err)
	}
	pub, priv, _ := ed25519Key(t)
	if err := cs.Save("AA:BB", "pid", pub, priv); err != nil {
		t.Fatal(err)
	}
	if err := cs.Forget("AA:BB"); err != nil {
		t.Fatal(err)
	}
	if saved := cs.Lookup("AA:BB"); saved != nil && saved.HasPairingCredentials() {
		t.Fatal("pairing survived Forget")
	}
}

// TestForgetIsCaseInsensitive covers the controller ruling that device IDs
// are case-insensitive: credentials saved under the lowercase deviceID a
// receiver's /info reports (e.g. the Android UxPlay port) must still be
// cleared when the bridge calls Forget with the uppercase ID it now
// normalizes every command to.
func TestForgetIsCaseInsensitive(t *testing.T) {
	cs, err := airplay.NewCredentialStore(filepath.Join(t.TempDir(), "c.json"))
	if err != nil {
		t.Fatal(err)
	}
	pub, priv, _ := ed25519Key(t)
	if err := cs.Save("aa:bb:cc:dd:ee:20", "pid", pub, priv); err != nil {
		t.Fatal(err)
	}
	if err := cs.Forget("AA:BB:CC:DD:EE:20"); err != nil {
		t.Fatal(err)
	}
	if saved := cs.Lookup("aa:bb:cc:dd:ee:20"); saved != nil && saved.HasPairingCredentials() {
		t.Fatal("pairing survived a Forget with different-case deviceID")
	}
}

func ed25519Key(t *testing.T) (ed25519.PublicKey, ed25519.PrivateKey, error) {
	t.Helper()
	return ed25519.GenerateKey(nil)
}
