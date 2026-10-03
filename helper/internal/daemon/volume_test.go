package daemon

import (
	"strings"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

func volumePtr(db float64) *float64 { return &db }

// waitStreaming waits until the only stream is streaming and video flows.
func waitStreaming(t *testing.T, d *Daemon, server *airplay.ReceiverServer) StreamInfo {
	t.Helper()
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		snap := d.Snapshot()
		if len(snap.Streams) == 1 && snap.Streams[0].State == StateStreaming && server.Stats().VideoPackets > 0 {
			return snap.Streams[0]
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("not streaming within 20 s: stats=%+v snapshot=%+v\n%s", server.Stats(), d.Snapshot(), goroutineDump())
	return StreamInfo{}
}

func TestConnectWithVolumeSetsReceiverVolumeAtStart(t *testing.T) {
	server, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, VolumeDB: volumePtr(-12)})
	if !resp.OK {
		t.Fatalf("connect: %+v", resp)
	}
	waitStreaming(t, d, server)
	if got := server.Stats().ParameterRequests; got < 2 {
		t.Fatalf("ParameterRequests = %d, want at least 2 (the level is sent twice at session start)", got)
	}
}

func TestConnectWithoutVolumeLeavesReceiverVolumeAlone(t *testing.T) {
	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	waitStreaming(t, d, server)
	time.Sleep(500 * time.Millisecond)
	if got := server.Stats().ParameterRequests; got != 0 {
		t.Fatalf("ParameterRequests = %d, want 0 when no volume was asked for", got)
	}
}

func TestConnectRejectsOutOfRangeVolume(t *testing.T) {
	_, d, addr := startEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	resp := d.HandleRequest(Request{Cmd: "connect", Target: addr.IP.String(), Port: addr.Port, VolumeDB: volumePtr(6)})
	if resp.OK || !strings.Contains(resp.Error, "-30 to 0") {
		t.Fatalf("connect with +6 dB = %+v, want a range error", resp)
	}
	if n := len(d.Snapshot().Streams); n != 0 {
		t.Fatalf("%d streams after a rejected connect, want 0", n)
	}
}

func TestVolumeRequestSetsLiveSessionVolume(t *testing.T) {
	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	info := waitStreaming(t, d, server)
	before := server.Stats().ParameterRequests
	if resp := d.HandleRequest(Request{Cmd: "volume", Target: info.DeviceIP, VolumeDB: volumePtr(-20)}); !resp.OK {
		t.Fatalf("volume: %+v", resp)
	}
	if got := server.Stats().ParameterRequests; got != before+1 {
		t.Fatalf("ParameterRequests = %d, want %d", got, before+1)
	}
}

func TestVolumeRequestValidation(t *testing.T) {
	server, d := setupEmbeddedDaemon(t, airplay.ReceiverProfileModern)
	info := waitStreaming(t, d, server)
	for _, tc := range []struct {
		name string
		req  Request
		want string
	}{
		{"no target", Request{Cmd: "volume", VolumeDB: volumePtr(-10)}, "volume requires a target"},
		{"no level", Request{Cmd: "volume", Target: info.DeviceIP}, "volume_db is required"},
		{"out of range", Request{Cmd: "volume", Target: info.DeviceIP, VolumeDB: volumePtr(-31)}, "-30 to 0"},
		{"unknown target", Request{Cmd: "volume", Target: "192.0.2.1", VolumeDB: volumePtr(-10)}, "no active stream to 192.0.2.1"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			resp := d.HandleRequest(tc.req)
			if resp.OK || !strings.Contains(resp.Error, tc.want) {
				t.Fatalf("%+v: got %+v, want an error containing %q", tc.req, resp, tc.want)
			}
		})
	}
}
