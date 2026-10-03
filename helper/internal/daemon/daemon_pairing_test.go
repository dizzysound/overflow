package daemon

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"net"
	"strings"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

func TestRequiredPairingCredentialKind(t *testing.T) {
	const (
		passwordFlag  = uint64(1 << 7)
		pinFlag       = uint64(1 << 9)
		modernFeature = airplay.FeatureSystemPairing | airplay.FeatureTransientPairing | uint64(1<<38) | uint64(1<<46)
		legacyFeature = modernFeature | uint64(1<<51)
	)

	for _, test := range []struct {
		name string
		info *airplay.ReceiverInfo
		want CredentialKind
	}{
		{name: "modern password is not an SRP credential", info: &airplay.ReceiverInfo{Features: modernFeature, StatusFlags: passwordFlag}},
		{name: "modern password suppresses advertised PIN", info: &airplay.ReceiverInfo{Features: modernFeature, StatusFlags: passwordFlag | pinFlag}},
		{name: "modern PIN only", info: &airplay.ReceiverInfo{Features: modernFeature, StatusFlags: pinFlag}, want: CredentialKindPIN},
		{name: "legacy password", info: &airplay.ReceiverInfo{Features: legacyFeature, StatusFlags: passwordFlag}, want: CredentialKindPassword},
		{name: "legacy password wins over PIN", info: &airplay.ReceiverInfo{Features: legacyFeature, StatusFlags: passwordFlag | pinFlag}, want: CredentialKindPassword},
	} {
		t.Run(test.name, func(t *testing.T) {
			if got := requiredPairingCredentialKind(test.info); got != test.want {
				t.Fatalf("requiredPairingCredentialKind() = %q, want %q", got, test.want)
			}
		})
	}
}

func TestRestoreSavedPairingRestoresRawProtocol(t *testing.T) {
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatalf("GenerateKey: %v", err)
	}
	client := airplay.NewAirPlayClient("127.0.0.1", 7000)
	err = restoreSavedPairing(client, &airplay.SavedCredentials{
		PairingID:       "pair-id",
		Ed25519Public:   pub,
		Ed25519Seed:     priv.Seed(),
		PairingProtocol: airplay.PairingProtocolRaw,
	})
	if err != nil {
		t.Fatalf("restoreSavedPairing: %v", err)
	}
	if got := client.PairingProtocol(); got != airplay.PairingProtocolRaw {
		t.Fatalf("restored protocol = %q, want raw", got)
	}
}

func TestHandleConnectSubmitsPINToPendingStream(t *testing.T) {
	const (
		target = "192.0.2.10"
		pin    = "1234"
	)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	client := airplay.NewAirPlayClient(target, 7000)
	credentialCh := make(chan string, 1)
	entry := &activeStream{
		deviceIP:     target,
		state:        StatePINRequired,
		client:       client,
		cancelFn:     cancel,
		credentialCh: credentialCh,
	}
	d := &Daemon{
		streams: map[string]*activeStream{target: entry},
	}

	pairingID := client.PairingID
	resp := d.handleConnect(Request{Cmd: "connect", Pin: pin})

	if !resp.OK {
		t.Fatalf("handleConnect() failed: %s", resp.Error)
	}
	if resp.State != StateConnecting {
		t.Fatalf("response state = %q, want %q", resp.State, StateConnecting)
	}
	if resp.Device != target {
		t.Fatalf("response device = %q, want %q", resp.Device, target)
	}
	if got := d.streams[target]; got != entry {
		t.Fatalf("pending stream was replaced: got %p, want %p", got, entry)
	}
	if entry.client != client {
		t.Fatalf("pending client was replaced: got %p, want %p", entry.client, client)
	}
	if entry.client.PairingID != pairingID {
		t.Fatalf("pairing ID changed from %q to %q", pairingID, entry.client.PairingID)
	}
	if entry.state != StateConnecting {
		t.Fatalf("entry state = %q, want %q", entry.state, StateConnecting)
	}
	if entry.credentialCh != credentialCh {
		t.Fatal("pending credential channel was replaced")
	}

	select {
	case got := <-credentialCh:
		if got != pin {
			t.Fatalf("submitted PIN = %q, want %q", got, pin)
		}
	default:
		t.Fatal("PIN was not submitted to the pending stream")
	}

	select {
	case <-ctx.Done():
		t.Fatal("claiming a pending credential session cancelled its connection context")
	default:
	}
}

func TestHandleConnectRejectsCredentialForNonPendingActiveTarget(t *testing.T) {
	const (
		pendingTarget = "192.0.2.10"
		requestTarget = "192.0.2.20"
	)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	credentialCh := make(chan string, 1)
	entry := &activeStream{
		deviceIP:     pendingTarget,
		state:        StatePINRequired,
		client:       airplay.NewAirPlayClient(pendingTarget, 7000),
		cancelFn:     cancel,
		credentialCh: credentialCh,
	}
	other := &activeStream{
		deviceIP: requestTarget,
		state:    StateStreaming,
	}
	d := &Daemon{
		streams: map[string]*activeStream{
			pendingTarget: entry,
			requestTarget: other,
		},
	}

	resp := d.handleConnect(Request{Cmd: "connect", Target: requestTarget, Pin: "1234"})

	if resp.OK {
		t.Fatal("handleConnect() accepted a credential for a non-pending target")
	}
	if resp.State != StatePINRequired {
		t.Fatalf("response state = %q, want %q", resp.State, StatePINRequired)
	}
	if !strings.Contains(resp.Error, "prompt is not ready") {
		t.Fatalf("response error = %q, want a prompt-not-ready error", resp.Error)
	}
	if got := d.streams[pendingTarget]; got != entry {
		t.Fatalf("pending stream changed: got %p, want %p", got, entry)
	}
	if entry.state != StatePINRequired {
		t.Fatalf("entry state = %q, want %q", entry.state, StatePINRequired)
	}
	select {
	case got := <-credentialCh:
		t.Fatalf("mismatched PIN %q was delivered to pending stream", got)
	default:
	}
	select {
	case <-ctx.Done():
		t.Fatal("target mismatch cancelled the pending connection")
	default:
	}
}

func TestHandleConnectRejectsTargetlessPINWithoutPendingSession(t *testing.T) {
	const streamingTarget = "192.0.2.30"
	d := &Daemon{
		streams: map[string]*activeStream{
			streamingTarget: {
				deviceIP: streamingTarget,
				state:    StateStreaming,
			},
		},
	}

	resp := d.handleConnect(Request{Cmd: "connect", Pin: "1234"})

	if resp.OK {
		t.Fatal("handleConnect() accepted a targetless PIN with no pending session")
	}
	if resp.State != StateStreaming {
		t.Fatalf("response state = %q, want existing state %q", resp.State, StateStreaming)
	}
	if !strings.Contains(resp.Error, "no device is waiting for a credential") {
		t.Fatalf("response error = %q, want no-pending-session error", resp.Error)
	}
	if len(d.streams) != 1 || d.streams[streamingTarget] == nil {
		t.Fatalf("streams changed after rejected PIN: %+v", d.streams)
	}
}

func TestTargetedDisconnectClearsPendingPINSession(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer listener.Close()

	accepted := make(chan net.Conn, 1)
	acceptErr := make(chan error, 1)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			acceptErr <- err
			return
		}
		accepted <- conn
	}()

	addr := listener.Addr().(*net.TCPAddr)
	client := airplay.NewAirPlayClient(addr.IP.String(), addr.Port)
	connectCtx, connectCancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer connectCancel()
	if err := client.Connect(connectCtx); err != nil {
		t.Fatalf("connect AirPlay client: %v", err)
	}
	defer client.Close()

	var peer net.Conn
	select {
	case peer = <-accepted:
		defer peer.Close()
	case err := <-acceptErr:
		t.Fatalf("accept: %v", err)
	case <-time.After(2 * time.Second):
		t.Fatal("timed out accepting AirPlay client connection")
	}

	streamCtx, streamCancel := context.WithCancel(context.Background())
	entry := &activeStream{
		deviceIP:     addr.IP.String(),
		state:        StatePINRequired,
		client:       client,
		cancelFn:     streamCancel,
		credentialCh: make(chan string, 1),
	}
	d := &Daemon{
		streams: map[string]*activeStream{entry.deviceIP: entry},
	}

	resp := d.handleDisconnect(Request{Cmd: "disconnect", Target: entry.deviceIP})

	if !resp.OK {
		t.Fatalf("handleDisconnect() failed: %s", resp.Error)
	}
	if resp.State != StateIdle {
		t.Fatalf("response state = %q, want %q", resp.State, StateIdle)
	}
	if _, ok := d.streams[entry.deviceIP]; ok {
		t.Fatalf("pending stream %q remains after disconnect", entry.deviceIP)
	}

	select {
	case <-streamCtx.Done():
	case <-time.After(time.Second):
		t.Fatal("targeted disconnect did not cancel the pending stream context")
	}

	if err := peer.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatalf("set peer deadline: %v", err)
	}
	buf := make([]byte, 1)
	if n, err := peer.Read(buf); err == nil {
		t.Fatalf("pending AirPlay connection remained open after disconnect (read %d bytes)", n)
	} else if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
		t.Fatal("pending AirPlay connection remained open until the read deadline")
	}
}

func TestHandleConnectTargetsOneOfMultiplePendingCredentials(t *testing.T) {
	const (
		firstTarget  = "192.0.2.10"
		secondTarget = "192.0.2.20"
		password     = "second password"
	)

	firstCh := make(chan string, 1)
	secondCh := make(chan string, 1)
	d := &Daemon{streams: map[string]*activeStream{
		firstTarget: {
			deviceIP:       firstTarget,
			state:          StatePINRequired,
			credentialKind: CredentialKindPIN,
			credentialCh:   firstCh,
		},
		secondTarget: {
			deviceIP:       secondTarget,
			state:          StatePINRequired,
			credentialKind: CredentialKindPassword,
			credentialCh:   secondCh,
		},
	}}

	resp := d.handleConnect(Request{Cmd: "connect", Target: secondTarget, Pin: password})
	if !resp.OK {
		t.Fatalf("targeted credential submission failed: %s", resp.Error)
	}
	if resp.State != StatePINRequired {
		t.Fatalf("aggregate state = %q, want %q while first target still waits", resp.State, StatePINRequired)
	}
	if d.streams[firstTarget].state != StatePINRequired {
		t.Fatalf("first target state = %q, want unchanged", d.streams[firstTarget].state)
	}
	if d.streams[secondTarget].state != StateConnecting {
		t.Fatalf("second target state = %q, want %q", d.streams[secondTarget].state, StateConnecting)
	}
	select {
	case got := <-secondCh:
		if got != password {
			t.Fatalf("second target received %q, want %q", got, password)
		}
	default:
		t.Fatal("second target did not receive its credential")
	}
	select {
	case got := <-firstCh:
		t.Fatalf("first target unexpectedly received %q", got)
	default:
	}
}

func TestHandleConnectRejectsAmbiguousTargetlessCredential(t *testing.T) {
	const (
		firstTarget  = "192.0.2.10"
		secondTarget = "192.0.2.20"
	)

	firstCh := make(chan string, 1)
	secondCh := make(chan string, 1)
	d := &Daemon{streams: map[string]*activeStream{
		firstTarget: {
			deviceIP:     firstTarget,
			state:        StatePINRequired,
			credentialCh: firstCh,
		},
		secondTarget: {
			deviceIP:     secondTarget,
			state:        StatePINRequired,
			credentialCh: secondCh,
		},
	}}

	resp := d.handleConnect(Request{Cmd: "connect", Pin: "1234"})
	if resp.OK {
		t.Fatal("targetless credential unexpectedly succeeded with two waiters")
	}
	if resp.State != StatePINRequired {
		t.Fatalf("response state = %q, want %q", resp.State, StatePINRequired)
	}
	if !strings.Contains(resp.Error, "multiple devices") || !strings.Contains(resp.Error, "specify a target") {
		t.Fatalf("response error = %q, want an ambiguity error", resp.Error)
	}
	for target, ch := range map[string]chan string{firstTarget: firstCh, secondTarget: secondCh} {
		select {
		case got := <-ch:
			t.Fatalf("target %s unexpectedly received %q", target, got)
		default:
		}
	}
}

func TestStatusSelectsDeterministicPendingCredential(t *testing.T) {
	d := &Daemon{streams: map[string]*activeStream{
		"192.0.2.20": {
			device:         "Password TV",
			deviceIP:       "192.0.2.20",
			state:          StatePINRequired,
			credentialKind: CredentialKindPassword,
			credentialCh:   make(chan string, 1),
		},
		"192.0.2.10": {
			device:         "PIN TV",
			deviceIP:       "192.0.2.10",
			state:          StatePINRequired,
			credentialKind: CredentialKindPIN,
			credentialCh:   make(chan string, 1),
		},
	}}

	resp := d.handleStatus()
	if resp.DeviceIP != "192.0.2.10" || resp.Device != "PIN TV" {
		t.Fatalf("top-level pending target = %q (%q), want deterministic first target", resp.Device, resp.DeviceIP)
	}
	if !resp.NeedsCredential || !resp.NeedsPIN || resp.CredentialKind != CredentialKindPIN {
		t.Fatalf("top-level credential metadata = %+v", resp)
	}
	if len(resp.Streams) != 2 || resp.Streams[0].DeviceIP != "192.0.2.10" || resp.Streams[1].DeviceIP != "192.0.2.20" {
		t.Fatalf("stream metadata is not sorted and complete: %+v", resp.Streams)
	}
}

func TestTargetedDisconnectLeavesOtherPendingCredential(t *testing.T) {
	const (
		firstTarget  = "192.0.2.10"
		secondTarget = "192.0.2.20"
	)

	firstCtx, cancelFirst := context.WithCancel(context.Background())
	secondCtx, cancelSecond := context.WithCancel(context.Background())
	defer cancelSecond()
	d := &Daemon{streams: map[string]*activeStream{
		firstTarget: {
			deviceIP:     firstTarget,
			state:        StatePINRequired,
			cancelFn:     cancelFirst,
			credentialCh: make(chan string, 1),
		},
		secondTarget: {
			deviceIP:       secondTarget,
			state:          StatePINRequired,
			cancelFn:       cancelSecond,
			credentialKind: CredentialKindPassword,
			credentialCh:   make(chan string, 1),
		},
	}}

	resp := d.handleDisconnect(Request{Cmd: "disconnect", Target: firstTarget})
	if !resp.OK || resp.State != StatePINRequired {
		t.Fatalf("targeted disconnect response = %+v, want other credential prompt preserved", resp)
	}
	if _, ok := d.streams[firstTarget]; ok {
		t.Fatalf("disconnected target %s remains", firstTarget)
	}
	if got := d.streams[secondTarget]; got == nil || got.state != StatePINRequired {
		t.Fatalf("other pending target changed: %+v", got)
	}
	select {
	case <-firstCtx.Done():
	default:
		t.Fatal("disconnected target context was not cancelled")
	}
	select {
	case <-secondCtx.Done():
		t.Fatal("targeted disconnect cancelled the other pending target")
	default:
	}
}

func TestAsyncConnectFailureIsReportedByStatus(t *testing.T) {
	listener, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve port: %v", err)
	}
	addr := listener.Addr().(*net.TCPAddr)
	if err := listener.Close(); err != nil {
		t.Fatalf("close reserved port: %v", err)
	}

	d, err := New(Config{
		CredBackend: "file",
		CredFile:    t.TempDir() + "/credentials.json",
		NoAudio:     true,
	})
	if err != nil {
		t.Fatalf("create daemon: %v", err)
	}
	resp := d.handleConnect(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port})
	if !resp.OK {
		t.Fatalf("queue connection: %+v", resp)
	}

	deadline := time.Now().Add(2 * time.Second)
	for {
		status := d.handleStatus()
		if status.State == StateIdle && status.Error != "" {
			if !strings.Contains(status.Error, "connect to") {
				t.Fatalf("asynchronous error = %q, want connection context", status.Error)
			}
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for asynchronous failure; last status: %+v", status)
		}
		time.Sleep(time.Millisecond)
	}
}

func TestAcceptedCredentialClearsPreviousAsyncError(t *testing.T) {
	const target = "192.0.2.10"
	credentialCh := make(chan string, 1)
	d := &Daemon{
		lastError:       "old asynchronous failure",
		lastErrorTarget: target,
		streams: map[string]*activeStream{
			target: {
				deviceIP:     target,
				state:        StatePINRequired,
				credentialCh: credentialCh,
			},
		},
	}

	resp := d.handleConnect(Request{Cmd: "connect", Target: target, Pin: "1234"})
	if !resp.OK {
		t.Fatalf("credential submission failed: %+v", resp)
	}
	if status := d.handleStatus(); status.Error != "" {
		t.Fatalf("status retained stale asynchronous error %q", status.Error)
	}
}

func TestCredentialForDifferentTargetPreservesAsyncError(t *testing.T) {
	const (
		failedTarget  = "192.0.2.10"
		pendingTarget = "192.0.2.20"
		wantError     = failedTarget + ": password pairing failed"
	)
	d := &Daemon{
		lastError:       wantError,
		lastErrorTarget: failedTarget,
		streams: map[string]*activeStream{
			pendingTarget: {
				deviceIP:     pendingTarget,
				state:        StatePINRequired,
				credentialCh: make(chan string, 1),
			},
		},
	}

	resp := d.handleConnect(Request{Cmd: "connect", Target: pendingTarget, Pin: "1234"})
	if !resp.OK {
		t.Fatalf("credential submission failed: %+v", resp)
	}
	if status := d.handleStatus(); status.Error != wantError {
		t.Fatalf("other target cleared asynchronous error: got %q, want %q", status.Error, wantError)
	}
}
