package bridge

import (
	"encoding/json"
	"io"
	"sync"
)

// eventWriter emits one JSON object per line on the plugin's stdout.
type eventWriter struct {
	mu  sync.Mutex
	out io.Writer
}

func (w *eventWriter) emit(v any) {
	line, err := json.Marshal(v)
	if err != nil {
		return
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	_, _ = w.out.Write(append(line, '\n'))
}

func (w *eventWriter) ready() {
	w.emit(struct {
		Event        string   `json:"event"`
		Version      int      `json:"version"`
		Capabilities []string `json:"capabilities"`
	}{"ready", 1, []string{"live_lead"}})
}

func (w *eventWriter) devices(ds []Device) {
	if ds == nil {
		ds = []Device{}
	}
	w.emit(struct {
		Event   string   `json:"event"`
		Devices []Device `json:"devices"`
	}{"devices", ds})
}

// room emits the canonical "display" event for a room state change. "room"
// is retired as an emitted event name (Revision 1.1); the internal Manager,
// RoomEvent and room terminology stay as-is to keep the fork thin.
func (w *eventWriter) room(e RoomEvent) {
	w.emit(struct {
		Event string `json:"event"`
		RoomEvent
	}{"display", e})
}

func (w *eventWriter) fatal(err error) {
	w.emit(struct {
		Event string `json:"event"`
		Error string `json:"error"`
	}{"fatal", err.Error()})
}

func (w *eventWriter) keyframe(deviceID, reason string) {
	w.emit(struct {
		Event    string `json:"event"`
		DeviceID string `json:"device_id"`
		Reason   string `json:"reason"`
	}{"keyframe", deviceID, reason})
}

func (w *eventWriter) delivery(s DeliverySummary) {
	w.emit(struct {
		Event    string `json:"event"`
		DeviceID string `json:"device_id"`
		Frames   int    `json:"frames"`
		P50Ms    int    `json:"p50_ms"`
		P99Ms    int    `json:"p99_ms"`
		Late     int    `json:"late"`
		LeadMs   int    `json:"lead_ms"`
		// Revision 1.4: the slide's target and the announced ceiling.
		LeadTargetMs  int `json:"lead_target_ms"`
		LeadCeilingMs int `json:"lead_ceiling_ms"`
		// Revision 1.3: audio packets lost (resend requested) and resent.
		AudioLost   int `json:"audio_lost"`
		AudioResent int `json:"audio_resent"`
		// Audio frames dropped at the sender as too late for the lead.
		AudioDropped int `json:"audio_dropped"`
		// Revision 1.5: audio RTP timeline discontinuities.
		AudioJumps int `json:"audio_jumps"`
	}{"delivery", s.DeviceID, s.Frames, s.P50Ms, s.P99Ms, s.Late, s.LeadMs, s.LeadTargetMs, s.LeadCeilingMs, s.AudioLost, s.AudioResent, s.AudioDropped, s.AudioJumps})
}
