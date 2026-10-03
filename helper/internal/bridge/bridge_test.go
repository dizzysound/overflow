package bridge

import (
	"bufio"
	"context"
	"encoding/json"
	"io"
	"path/filepath"
	"testing"
	"time"
)

func startBridge(t *testing.T) (*MessageWriter, *bufio.Scanner, <-chan error, io.Closer) {
	t.Helper()
	inR, inW := io.Pipe()
	outR, outW := io.Pipe()
	done := make(chan error, 1)
	go func() {
		done <- Run(context.Background(), Config{
			CredBackend:  "file",
			CredFile:     filepath.Join(t.TempDir(), "c.json"),
			TickInterval: 20 * time.Millisecond,
		}, inR, outW)
		outW.Close()
	}()
	t.Cleanup(func() { inW.Close() })
	return NewMessageWriter(inW), bufio.NewScanner(outR), done, inW
}

func nextEvent(t *testing.T, sc *bufio.Scanner) map[string]any {
	t.Helper()
	if !sc.Scan() {
		t.Fatalf("event stream ended: %v", sc.Err())
	}
	var ev map[string]any
	if err := json.Unmarshal(sc.Bytes(), &ev); err != nil {
		t.Fatalf("bad event line %q: %v", sc.Text(), err)
	}
	return ev
}

func TestBridgeSaysReadyAndExitsOnEOF(t *testing.T) {
	w, sc, done, in := startBridge(t)
	_ = w
	if ev := nextEvent(t, sc); ev["event"] != "ready" || ev["version"] != float64(1) {
		t.Fatalf("first event = %v", ev)
	}
	in.Close()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run returned %v on EOF, want nil", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Run did not exit on EOF")
	}
}

func TestBridgeRejectsMediaBeforeHello(t *testing.T) {
	w, sc, done, _ := startBridge(t)
	nextEvent(t, sc) // ready
	w.Write(MsgVideoAU, EncodeMedia(Media{CaptureNs: 1, SendNs: 1, Data: []byte{0}}))
	if ev := nextEvent(t, sc); ev["event"] != "fatal" {
		t.Fatalf("event = %v, want fatal", ev)
	}
	if err := <-done; err == nil {
		t.Fatal("Run returned nil after protocol violation")
	}
}

func TestBridgeRejectsBadHello(t *testing.T) {
	w, sc, done, _ := startBridge(t)
	nextEvent(t, sc)
	h := DefaultHello()
	h.Audio.SampleRate = 48000
	b, _ := json.Marshal(h)
	w.Write(MsgHello, b)
	if ev := nextEvent(t, sc); ev["event"] != "fatal" {
		t.Fatalf("event = %v, want fatal", ev)
	}
	<-done
}

func TestBridgeShutdownCommand(t *testing.T) {
	w, sc, done, _ := startBridge(t)
	nextEvent(t, sc)
	b, _ := json.Marshal(DefaultHello())
	w.Write(MsgHello, b)
	w.Write(MsgCommand, []byte(`{"cmd":"shutdown"}`))
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("shutdown returned %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("shutdown command ignored")
	}
}

// TestBridgeIgnoresCredentialForUnknownDevice guards against a bug where a
// credential command for a device_id the Manager has no IP for (not selected,
// or not yet discovered) fell through to daemon.HandleRequest with an empty
// Target. The daemon treats an empty Target as a legacy fallback: with a
// non-empty PIN it can deliver the PIN to some other waiting receiver, and
// with an empty PIN it can start streaming to an arbitrary unselected device
// the Manager never tracks. This test can't observe the absence of that
// daemon call directly, so it mainly guards the no-crash/no-fatal path: the
// credential command must be silently ignored (logged to stderr) and Run
// must continue on to process the following shutdown command normally.
func TestBridgeIgnoresCredentialForUnknownDevice(t *testing.T) {
	w, sc, done, _ := startBridge(t)
	nextEvent(t, sc) // ready
	b, _ := json.Marshal(DefaultHello())
	w.Write(MsgHello, b)
	w.Write(MsgCommand, []byte(`{"cmd":"credential","device_id":"unknown-device","value":"1234"}`))
	w.Write(MsgCommand, []byte(`{"cmd":"shutdown"}`))
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Run returned %v after ignored credential + shutdown, want nil", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("Run did not exit after shutdown following ignored credential")
	}
}

func TestParsePortRange(t *testing.T) {
	if lo, hi, err := ParsePortRange(""); err != nil || lo != 0 || hi != 0 {
		t.Fatalf("empty: %d %d %v", lo, hi, err)
	}
	if lo, hi, err := ParsePortRange("60000-60010"); err != nil || lo != 60000 || hi != 60010 {
		t.Fatalf("range: %d %d %v", lo, hi, err)
	}
	for _, bad := range []string{"60000", "b-c", "60010-60000", "60000-60001"} {
		if _, _, err := ParsePortRange(bad); err == nil {
			t.Fatalf("%q accepted", bad)
		}
	}
}
