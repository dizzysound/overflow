package airplay

import (
	"context"
	"encoding/binary"
	"net"
	"testing"
	"time"
)

func TestReceiverClockGateHoldsFirstFrameUntilSyncOrDeadline(t *testing.T) {
	var none *receiverClockGate
	if !none.allowsFirstFrame(time.Now()) {
		t.Fatal("nil gate (non-legacy session) must never hold video")
	}
	none.markSynced() // must not panic

	start := time.Now()
	g := newReceiverClockGate(start.Add(receiverClockSyncWait))
	if g.allowsFirstFrame(start) {
		t.Fatal("gate allowed the first frame before the receiver's NTP exchange")
	}
	if g.allowsFirstFrame(start.Add(receiverClockSyncWait - time.Millisecond)) {
		t.Fatal("gate allowed the first frame before its deadline")
	}
	g.markSynced()
	g.markSynced() // idempotent
	if !g.allowsFirstFrame(start) {
		t.Fatal("gate held the first frame after the receiver's clock synced")
	}

	late := newReceiverClockGate(start.Add(receiverClockSyncWait))
	if !late.allowsFirstFrame(start.Add(receiverClockSyncWait)) {
		t.Fatal("gate must fall back to unsynced video at its deadline")
	}
}

func TestNTPTimingResponderSignalsReplyOnlyForRequests(t *testing.T) {
	server, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	client, err := net.ListenPacket("udp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	g := newReceiverClockGate(time.Now().Add(time.Hour))
	go ntpTimingResponder(ctx, server, g.markSynced)

	// A 0xd3 response (e.g. to our own probe) must not count as a sync.
	response := make([]byte, 32)
	response[0], response[1] = 0x80, 0xd3
	if _, err := client.WriteTo(response, server.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	time.Sleep(50 * time.Millisecond)
	if g.allowsFirstFrame(time.Now()) {
		t.Fatal("a timing response was treated as the receiver's timing request")
	}

	request := make([]byte, 32)
	request[0], request[1] = 0x80, 0xd2
	binary.BigEndian.PutUint64(request[24:32], 0x0123456789abcdef)
	if _, err := client.WriteTo(request, server.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	reply := make([]byte, 64)
	_ = client.SetReadDeadline(time.Now().Add(2 * time.Second))
	n, _, err := client.ReadFrom(reply)
	if err != nil || n != 32 || reply[1] != 0xd3 {
		t.Fatalf("timing reply = %d bytes %x, err %v", n, reply[:n], err)
	}
	deadline := time.Now().Add(time.Second)
	for !g.allowsFirstFrame(time.Now()) && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if !g.allowsFirstFrame(time.Now()) {
		t.Fatal("answered timing request did not mark the receiver clock synced")
	}
}

// TestLegacySessionGatesVideoOnReceiverClock checks the wiring: legacy AES-CTR
// sessions get a gate that the receiver's NTP request opens; HAP/ChaCha
// sessions get none.
func TestLegacySessionGatesVideoOnReceiverClock(t *testing.T) {
	for _, test := range []struct {
		profile  ReceiverProfile
		wantGate bool
	}{
		{ReceiverProfileAppleTV3, true},
		{ReceiverProfileUxPlay, true},
		{ReceiverProfileModern, false},
	} {
		t.Run(string(test.profile), func(t *testing.T) {
			_, client, ctx := newReceiverServerTestPair(t, ReceiverConfig{Profile: test.profile})
			if err := client.Pair(ctx, ""); err != nil {
				t.Fatalf("pair: %v", err)
			}
			if client.info.SupportsFairPlaySAP() {
				if err := client.FairPlaySetup(ctx); err != nil {
					t.Fatalf("FairPlay setup: %v", err)
				}
			}
			session, err := client.SetupMirror(ctx, StreamConfig{NoAudio: true})
			if err != nil {
				t.Fatalf("setup mirror: %v", err)
			}
			defer session.Close()
			if (session.receiverClock != nil) != test.wantGate {
				t.Fatalf("receiver clock gate present = %t, want %t", session.receiverClock != nil, test.wantGate)
			}
			if !test.wantGate {
				return
			}
			// The test receivers probe the sender's timing port during SETUP,
			// as UxPlay does, so the gate must open without waiting for its
			// fallback deadline.
			deadline := time.Now().Add(2 * time.Second)
			for !session.receiverClock.allowsFirstFrame(time.Now()) && time.Now().Before(deadline) {
				time.Sleep(time.Millisecond)
			}
			select {
			case <-session.receiverClock.ready:
			default:
				t.Fatal("receiver's NTP request did not open the legacy video gate")
			}
		})
	}
}
