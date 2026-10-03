package bridge_test

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"path/filepath"
	"runtime/pprof"
	"strings"
	"sync"
	"testing"
	"time"

	"doubletake/internal/airplay"
	"doubletake/internal/bridge"
	"doubletake/internal/testfeed"
	"doubletake/internal/testmedia"
)

type harness struct {
	w      *bridge.MessageWriter
	events chan map[string]any
	done   chan error
	in     io.Closer

	trailMu sync.Mutex
	trail   []string // recent display events, for timeout reports
}

// harnessTrailLen bounds the display-event history a timeout report prints.
const harnessTrailLen = 40

func (h *harness) record(line []byte) {
	h.trailMu.Lock()
	defer h.trailMu.Unlock()
	h.trail = append(h.trail, time.Now().Format("15:04:05.000")+" "+string(line))
	if len(h.trail) > harnessTrailLen {
		h.trail = h.trail[len(h.trail)-harnessTrailLen:]
	}
}

// timeout fails the test with the display events seen so far and a goroutine
// dump. Timeouts in these loopback tests were intermittent on macOS and the
// bare "timed out" message did not say whether a room stalled or was retrying.
func (h *harness) timeout(t *testing.T, format string, args ...any) {
	t.Helper()
	h.trailMu.Lock()
	trail := strings.Join(h.trail, "\n")
	h.trailMu.Unlock()
	t.Fatalf("%s\nrecent display events:\n%s\n%s", fmt.Sprintf(format, args...), trail, goroutineDump())
}

// goroutineDump returns every goroutine's stack, grouped by identical stacks.
func goroutineDump() string {
	var b bytes.Buffer
	b.WriteString("goroutines:\n")
	_ = pprof.Lookup("goroutine").WriteTo(&b, 1)
	return b.String()
}

func startHarness(t *testing.T, ctx context.Context, extra []bridge.Device) *harness {
	t.Helper()
	inR, inW := io.Pipe()
	outR, outW := io.Pipe()
	h := &harness{w: bridge.NewMessageWriter(inW), events: make(chan map[string]any, 256), done: make(chan error, 1), in: inW}
	go func() {
		h.done <- bridge.Run(ctx, bridge.Config{
			CredBackend:  "file",
			CredFile:     filepath.Join(t.TempDir(), "c.json"),
			TickInterval: 50 * time.Millisecond,
			ExtraDevices: extra,
		}, inR, outW)
		outW.Close()
	}()
	go func() {
		sc := bufio.NewScanner(outR)
		for sc.Scan() {
			var ev map[string]any
			if json.Unmarshal(sc.Bytes(), &ev) == nil {
				if ev["event"] == "display" {
					h.record(sc.Bytes())
				}
				h.events <- ev
			}
		}
		close(h.events)
	}()
	helloSent := make(chan struct{})
	go testfeed.Feed(ctx, h.w, helloSent)
	<-helloSent
	return h
}

// waitRoom returns the first room event for id whose state is want, failing on
// timeout. Other events are skipped.
func (h *harness) waitRoom(t *testing.T, id, want string, timeout time.Duration) map[string]any {
	t.Helper()
	deadline := time.After(timeout)
	for {
		select {
		case ev, ok := <-h.events:
			if !ok {
				t.Fatalf("event stream closed waiting for %s=%s", id, want)
			}
			if ev["event"] == "fatal" {
				t.Fatalf("fatal: %v", ev)
			}
			if ev["event"] == "display" && ev["device_id"] == id && ev["state"] == want {
				return ev
			}
		case <-deadline:
			h.timeout(t, "timed out waiting for room %s state %s", id, want)
		}
	}
}

// waitLiveAnswering answers every credential prompt for id with value until the
// room is live. Roku's combined auth can prompt twice: once at pair-setup and
// again for the RTSP Digest challenge.
func (h *harness) waitLiveAnswering(t *testing.T, id, wantKind, value string, timeout time.Duration) {
	t.Helper()
	deadline := time.After(timeout)
	prompts := 0
	for {
		select {
		case ev, ok := <-h.events:
			if !ok {
				t.Fatalf("event stream closed waiting for %s to go live", id)
			}
			if ev["event"] == "fatal" {
				t.Fatalf("fatal: %v", ev)
			}
			if ev["event"] != "display" || ev["device_id"] != id {
				continue
			}
			switch ev["state"] {
			case "credential":
				if ev["credential_kind"] != wantKind {
					t.Fatalf("credential_kind = %v, want %s", ev["credential_kind"], wantKind)
				}
				prompts++
				h.command(t, bridge.Command{Cmd: "credential", DeviceID: id, Value: value})
			case "live":
				if prompts == 0 {
					t.Fatal("went live without asking for a credential")
				}
				return
			case "failed", "retrying":
				t.Fatalf("room %s: %v", id, ev)
			}
		case <-deadline:
			h.timeout(t, "timed out waiting for %s to go live", id)
		}
	}
}

func (h *harness) command(t *testing.T, c bridge.Command) {
	t.Helper()
	b, _ := json.Marshal(c)
	if err := h.w.Write(bridge.MsgCommand, b); err != nil {
		t.Fatal(err)
	}
}

func startReceiver(t *testing.T, ctx context.Context, cfg airplay.ReceiverConfig) (*airplay.ReceiverServer, bridge.Device) {
	t.Helper()
	srv, dev, err := listenReceiver(t, ctx, cfg, "127.0.0.1")
	if err != nil {
		t.Fatal(err)
	}
	return srv, dev
}

// listenReceiver starts an in-process receiver on host with an ephemeral port.
func listenReceiver(t *testing.T, ctx context.Context, cfg airplay.ReceiverConfig, host string) (*airplay.ReceiverServer, bridge.Device, error) {
	t.Helper()
	cfg.ListenAddress = net.JoinHostPort(host, "0")
	cfg.Logger = log.New(io.Discard, "", 0)
	srv, err := airplay.NewReceiverServer(cfg)
	if err != nil {
		return nil, bridge.Device{}, err
	}
	go srv.Serve(ctx)
	t.Cleanup(func() { srv.Close() })
	addr := srv.Addr().(*net.TCPAddr)
	return srv, bridge.Device{DeviceID: cfg.DeviceID, Name: cfg.Name, IP: addr.IP.String(), Port: addr.Port}, nil
}

// waitMedia waits until video and audio reach the receiver. The in-process
// receiver validates video decryption only for the legacy AppleTV3/UxPlay
// profiles (receiver_media.go drainVideo); Modern and Roku profiles discard
// payloads, so flow is asserted via packets and the SPS-derived canvas.
// Decryption correctness is covered by Task 7's AppleTV3-profile daemon test.
func waitMedia(t *testing.T, srv *airplay.ReceiverServer) {
	t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		st := srv.Stats()
		if st.VideoPackets > 0 && st.VideoWidth == testmedia.ClipWidth && st.VideoHeight == testmedia.ClipHeight && st.AudioPackets > 0 {
			if st.VideoCryptoErrors != 0 {
				t.Fatalf("video crypto errors: %d", st.VideoCryptoErrors)
			}
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("receiver got no media: %+v", srv.Stats())
}

func TestLoopbackPINPairingStreamsVideoAndAudio(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthPIN, Code: "1234",
		Name: "Narthex", DeviceID: "AA:BB:CC:DD:EE:01",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{{DeviceID: dev.DeviceID, AutoReconnect: true}}})

	ev := h.waitRoom(t, dev.DeviceID, "credential", 15*time.Second)
	if ev["credential_kind"] != "pin" {
		t.Fatalf("credential_kind = %v, want pin", ev["credential_kind"])
	}
	h.command(t, bridge.Command{Cmd: "credential", DeviceID: dev.DeviceID, Value: "1234"})
	h.waitRoom(t, dev.DeviceID, "live", 20*time.Second)
	waitMedia(t, srv)

	h.in.Close()
	select {
	case err := <-h.done:
		if err != nil {
			t.Fatalf("Run on EOF: %v", err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("helper did not exit on stdin EOF")
	}
}

func TestLoopbackRokuPasswordStreams(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileRoku, Auth: airplay.ReceiverAuthCombined, Code: "sacristy",
		Name: "Sacristy", DeviceID: "AA:BB:CC:DD:EE:02",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{{DeviceID: dev.DeviceID, AutoReconnect: true}}})

	h.waitLiveAnswering(t, dev.DeviceID, "password", "sacristy", 30*time.Second)
	waitMedia(t, srv)
}

func TestLoopbackReceiverLossRetries(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv, dev := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Friendship", DeviceID: "AA:BB:CC:DD:EE:03",
	})
	h := startHarness(t, ctx, []bridge.Device{dev})
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{{DeviceID: dev.DeviceID, AutoReconnect: true}}})
	h.waitRoom(t, dev.DeviceID, "live", 20*time.Second)

	srv.Close()
	h.waitRoom(t, dev.DeviceID, "retrying", 20*time.Second)
}

// TestLoopbackTwoRoomsIsolated streams to two receivers at once and checks that
// losing one leaves the other live and still receiving video. The daemon keys
// streams by IP, so the second receiver needs its own loopback address.
func TestLoopbackTwoRoomsIsolated(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srvB, devB, err := listenReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Fellowship", DeviceID: "AA:BB:CC:DD:EE:05",
	}, "127.0.0.2")
	if err != nil {
		t.Skipf("cannot bind 127.0.0.2 (%v); on macOS enable it with: sudo ifconfig lo0 alias 127.0.0.2", err)
	}
	srvA, devA := startReceiver(t, ctx, airplay.ReceiverConfig{
		Profile: airplay.ReceiverProfileModern, Auth: airplay.ReceiverAuthNone,
		Name: "Nave", DeviceID: "AA:BB:CC:DD:EE:04",
	})
	h := startHarness(t, ctx, []bridge.Device{devA, devB})
	h.command(t, bridge.Command{Cmd: "set_rooms", Rooms: []bridge.RoomSelection{
		{DeviceID: devA.DeviceID, AutoReconnect: true},
		{DeviceID: devB.DeviceID, AutoReconnect: true},
	}})

	live := map[string]bool{}
	deadline := time.After(30 * time.Second)
	for !live[devA.DeviceID] || !live[devB.DeviceID] {
		select {
		case ev, ok := <-h.events:
			if !ok {
				t.Fatal("event stream closed waiting for both rooms to go live")
			}
			if ev["event"] == "fatal" {
				t.Fatalf("fatal: %v", ev)
			}
			if ev["event"] == "display" && ev["state"] == "live" {
				live[ev["device_id"].(string)] = true
			}
		case <-deadline:
			h.timeout(t, "timed out waiting for both rooms to go live: %v", live)
		}
	}
	waitMedia(t, srvA)
	waitMedia(t, srvB)

	srvA.Close()
	samples := []uint64{srvB.Stats().VideoPackets}
	sampler := time.NewTicker(time.Second)
	defer sampler.Stop()
	deadline = time.After(20 * time.Second)
	sawRetry := false
	for !sawRetry || len(samples) < 3 {
		select {
		case ev, ok := <-h.events:
			if !ok {
				t.Fatal("event stream closed after receiver A closed")
			}
			if ev["event"] == "fatal" {
				t.Fatalf("fatal: %v", ev)
			}
			if ev["event"] != "display" {
				continue
			}
			if ev["device_id"] == devB.DeviceID && ev["state"] != "live" {
				t.Fatalf("room B left live after receiver A closed: %v", ev)
			}
			if ev["device_id"] == devA.DeviceID && ev["state"] == "retrying" {
				sawRetry = true
			}
		case <-sampler.C:
			if len(samples) < 3 {
				samples = append(samples, srvB.Stats().VideoPackets)
			}
		case <-deadline:
			h.timeout(t, "timed out: room A retrying=%v, B video samples=%v", sawRetry, samples)
		}
	}
	for i := 1; i < len(samples); i++ {
		if samples[i] <= samples[i-1] {
			t.Fatalf("room B video stalled after receiver A closed: VideoPackets samples %v", samples)
		}
	}
}
