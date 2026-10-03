package daemon

import (
	"context"
	"time"

	"doubletake/internal/airplay"
)

// ExternalMedia replaces GStreamer capture with media produced by another
// process (the OBS plugin). Every receiver shares one H.264 stream.
type ExternalMedia struct {
	Video *airplay.BroadcastCapture
	Audio *airplay.ExternalAudioSource
	// MinimumVideoLead is the upstream encoder's worst-case latency, reported
	// to the AirPlay latency policy. Zero is acceptable for low-latency NVENC.
	MinimumVideoLead time.Duration
	// RequestKeyframe (optional) asks the upstream encoder for an IDR on behalf
	// of deviceID. reason is "backlog", "join" or "receiver".
	RequestKeyframe func(deviceID, reason string)
	// RetryKeyframe (optional) asks again for a display still waiting for an
	// IDR: its first frame ("join") or the end of a shed ("backlog"). The
	// caller backs these off per display. Without it the daemon never re-asks.
	RetryKeyframe func(deviceID, reason string)
	// FrameDelivered (optional) is called once per video frame written to
	// deviceID, with that display's playout lead and the age from OBS capture
	// to write completion. lead is zero when the display has no per-display lead.
	FrameDelivered func(deviceID string, lead airplay.LeadState, age time.Duration)
	// AudioResend (optional) is called for each receiver retransmit request on
	// deviceID's audio stream: packets requested (lost) and packets resent.
	AudioResend func(deviceID string, requested, resent int)
	// AudioDropped (optional) is called for each audio frame on deviceID's
	// session dropped as too late for its lead.
	AudioDropped func(deviceID string)
	// AudioJump (optional) is called for each RTP timeline discontinuity on
	// deviceID's audio stream (samples: the size, positive for a gap).
	AudioJump func(deviceID string, samples int)
}

// RunEmbedded runs background discovery for callers that drive the daemon
// in-process through HandleRequest instead of the Unix control socket. It
// blocks until ctx is cancelled; call Shutdown afterwards.
func (d *Daemon) RunEmbedded(ctx context.Context) error {
	airplay.SetDebugMode(d.cfg.Debug)
	discoverCtx, discoverCancel := context.WithCancel(ctx)
	d.mu.Lock()
	if d.shuttingDown {
		d.mu.Unlock()
		discoverCancel()
		return nil
	}
	d.discoverCancel = discoverCancel
	d.mu.Unlock()
	go d.backgroundDiscover(discoverCtx)
	<-ctx.Done()
	return nil
}

// HandleRequest executes one control request in-process.
func (d *Daemon) HandleRequest(req Request) Response {
	return d.handleRequest(req)
}

// Snapshot is a consistent view of discovery and stream state.
type Snapshot struct {
	Devices         []DeviceInfo
	Streams         []StreamInfo
	LastError       string
	LastErrorTarget string
}

func (d *Daemon) Snapshot() Snapshot {
	d.mu.Lock()
	defer d.mu.Unlock()
	resp := d.statusResponseLocked(true, "")
	return Snapshot{
		Devices:         toDeviceInfos(d.devices),
		Streams:         resp.Streams,
		LastError:       d.lastError,
		LastErrorTarget: d.lastErrorTarget,
	}
}

// Forget deletes stored pairing for deviceID.
func (d *Daemon) Forget(deviceID string) error {
	return d.credStore.Forget(deviceID)
}
