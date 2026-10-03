// Package bridge connects the OBS plugin to doubletake's multi-target sender:
// it decodes the plugin's stdin protocol, feeds the external media sources,
// drives rooms through the embedded daemon, and reports state on stdout.
package bridge

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"sync"
	"time"
)

// Message types on stdin (plugin -> helper). See docs/protocol.md.
const (
	MsgHello    byte = 0x01
	MsgVideoAU  byte = 0x02
	MsgAudioPCM byte = 0x03
	MsgCommand  byte = 0x10

	FlagKeyframe byte = 0x01

	// MaxMessage bounds one message (type byte + payload).
	MaxMessage = 16 << 20

	mediaHeaderLen = 17 // capture_ns(8) + send_ns(8) + flags(1)
)

var ErrMessageTooLarge = errors.New("bridge: message exceeds 16 MiB")

type Message struct {
	Type    byte
	Payload []byte
}

// ReadMessage reads one frame: u32 little-endian length (type byte + payload),
// then the type byte, then the payload. It returns io.EOF only when r ends
// exactly on a frame boundary.
func ReadMessage(r io.Reader) (Message, error) {
	var lenBuf [4]byte
	if _, err := io.ReadFull(r, lenBuf[:]); err != nil {
		if err == io.ErrUnexpectedEOF {
			return Message{}, fmt.Errorf("bridge: truncated length: %w", err)
		}
		return Message{}, err
	}
	n := binary.LittleEndian.Uint32(lenBuf[:])
	if n == 0 {
		return Message{}, errors.New("bridge: zero-length message")
	}
	if n > MaxMessage {
		return Message{}, ErrMessageTooLarge
	}
	buf := make([]byte, n)
	if _, err := io.ReadFull(r, buf); err != nil {
		return Message{}, fmt.Errorf("bridge: truncated message: %w", err)
	}
	return Message{Type: buf[0], Payload: buf[1:]}, nil
}

// MessageWriter frames messages onto w. It is safe for concurrent use.
type MessageWriter struct {
	mu sync.Mutex
	w  io.Writer
}

func NewMessageWriter(w io.Writer) *MessageWriter { return &MessageWriter{w: w} }

func (mw *MessageWriter) Write(typ byte, payload []byte) error {
	if len(payload)+1 > MaxMessage {
		return ErrMessageTooLarge
	}
	frame := make([]byte, 5+len(payload))
	binary.LittleEndian.PutUint32(frame[:4], uint32(len(payload)+1))
	frame[4] = typ
	copy(frame[5:], payload)
	mw.mu.Lock()
	defer mw.mu.Unlock()
	_, err := mw.w.Write(frame)
	return err
}

// Media is the payload of MsgVideoAU and MsgAudioPCM. CaptureNs and SendNs are
// on the plugin's monotonic clock (OBS os_gettime_ns). Data is one Annex-B
// access unit (video) or interleaved S16LE stereo PCM (audio).
type Media struct {
	CaptureNs uint64
	SendNs    uint64
	Keyframe  bool
	Data      []byte
}

func EncodeMedia(m Media) []byte {
	out := make([]byte, mediaHeaderLen+len(m.Data))
	binary.LittleEndian.PutUint64(out[0:8], m.CaptureNs)
	binary.LittleEndian.PutUint64(out[8:16], m.SendNs)
	if m.Keyframe {
		out[16] = FlagKeyframe
	}
	copy(out[mediaHeaderLen:], m.Data)
	return out
}

func DecodeMedia(p []byte) (Media, error) {
	if len(p) < mediaHeaderLen+1 {
		return Media{}, fmt.Errorf("bridge: media payload is %d bytes, want at least %d", len(p), mediaHeaderLen+1)
	}
	return Media{
		CaptureNs: binary.LittleEndian.Uint64(p[0:8]),
		SendNs:    binary.LittleEndian.Uint64(p[8:16]),
		Keyframe:  p[16]&FlagKeyframe != 0,
		Data:      p[mediaHeaderLen:],
	}, nil
}

// mediaTime records m's send stamp in cm and returns m's capture time on the
// helper clock. A capture after the send stamp is treated as sent at capture.
func mediaTime(cm *clockMap, now time.Time, m Media) time.Time {
	capture := m.CaptureNs
	if m.SendNs < capture {
		capture = m.SendNs
	}
	cm.Observe(now, m.SendNs)
	return cm.Map(capture)
}

// Hello is the first message on stdin (JSON payload of MsgHello).
type Hello struct {
	Version int         `json:"version"`
	Video   VideoFormat `json:"video"`
	Audio   AudioFormat `json:"audio"`
}

type VideoFormat struct {
	Codec string `json:"codec"`
}

type AudioFormat struct {
	SampleRate int    `json:"sample_rate"`
	Channels   int    `json:"channels"`
	Format     string `json:"format"`
}

func DefaultHello() Hello {
	return Hello{
		Version: 1,
		Video:   VideoFormat{Codec: "h264"},
		Audio:   AudioFormat{SampleRate: 44100, Channels: 2, Format: "s16le"},
	}
}

func (h Hello) Validate() error {
	if h.Version != 1 {
		return fmt.Errorf("unsupported protocol version %d (helper speaks 1)", h.Version)
	}
	if h.Video.Codec != "h264" {
		return fmt.Errorf("unsupported video codec %q (want h264)", h.Video.Codec)
	}
	if h.Audio.SampleRate != 44100 || h.Audio.Channels != 2 || h.Audio.Format != "s16le" {
		return fmt.Errorf("unsupported audio format %+v (want 44100 Hz, 2 ch, s16le)", h.Audio)
	}
	return nil
}

// RoomSelection is one receiver the plugin wants live.
type RoomSelection struct {
	DeviceID      string `json:"device_id"`
	AutoReconnect bool   `json:"auto_reconnect"`
	// VolumeDB (optional, -30 to 0 dB) is set at the start of each session for
	// this room, overriding -volume. nil sends no volume command.
	VolumeDB *float64 `json:"volume_db,omitempty"`
	// Audio (optional) selects video-only streaming for this display: nil or
	// true is today's behavior (audio follows the session's normal path);
	// false streams no audio for the session (the SETUP still negotiates an audio
	// session, as with -no-audio, but no audio is ever sent), and its live events carry
	// "audio":"off" with an audio_reason of "turned off for this display".
	// The value applies at session start; a change for a live display takes
	// effect on the next restart.
	Audio *bool `json:"audio,omitempty"`
	// WifiTolerant (optional) lets this display's relay ride out brief network
	// stalls: nil or false is today's behavior; true raises its sink's
	// frame-queue budget from 67 ms to 250 ms, trading up to ~0.25 s extra
	// delay during a stall for fewer freezes until the next IDR. The value
	// applies at session start; a change for a live display takes effect on
	// the next restart.
	WifiTolerant *bool `json:"wifi_tolerant,omitempty"`
	// LatencyMs (optional, revision 1.2) is this display's playout lead in ms,
	// chosen by the plugin. nil or 0 keeps the helper's global lead and the
	// 67/250 ms relay budgets. Applies at session start.
	LatencyMs *int `json:"latency_ms,omitempty"`
	// LeadHeadroomMs (optional, revision 1.4) lets this display's lead slide
	// live up to latency_ms + lead_headroom_ms (0-500). Applies at session start.
	LeadHeadroomMs *int `json:"lead_headroom_ms,omitempty"`
	// AudioFormat (optional, revision 1.3) forces this display's screen-audio
	// codec: "alac" or "aac-eld". Absent, "" or "auto" keeps the helper's
	// choice from the receiver's /info. Applies at session start.
	AudioFormat string `json:"audio_format,omitempty"`
	// IP and Port (optional) address the receiver directly, for receivers
	// discovery cannot see. Port 0 means 7000.
	IP   string `json:"ip,omitempty"`
	Port int    `json:"port,omitempty"`
}

// Command is the JSON payload of MsgCommand. Displays (cmd "set_displays")
// is the canonical name for the room selection; Rooms (cmd "set_rooms") is
// kept as an alias for older clients. See resolveDisplays.
type Command struct {
	Cmd      string          `json:"cmd"`
	Displays []RoomSelection `json:"displays,omitempty"`
	Rooms    []RoomSelection `json:"rooms,omitempty"`
	DeviceID string          `json:"device_id,omitempty"`
	Value    string          `json:"value,omitempty"`
	VolumeDB *float64        `json:"volume_db,omitempty"` // set_volume
	LeadMs   *int            `json:"lead_ms,omitempty"`   // set_lead (revision 1.4)
}

// resolveDisplays picks the room selection a set_displays/set_rooms command
// carries: Displays (the canonical "displays" key) when the JSON key was
// present, else Rooms (the "rooms" alias). If both keys appear in the JSON,
// Displays wins, even when it is an explicit empty array -- encoding/json
// decodes an explicit "displays":[] into a non-nil, zero-length slice
// (distinct from an absent key, which decodes to nil), so presence must be
// checked with a nil check, not a length check, or an empty "displays"
// sent alongside a non-empty "rooms" would wrongly fall back to rooms.
func resolveDisplays(c Command) []RoomSelection {
	if c.Displays != nil {
		return c.Displays
	}
	return c.Rooms
}
