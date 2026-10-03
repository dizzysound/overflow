package bridge

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"testing"
	"time"
)

func TestMessageRoundTrip(t *testing.T) {
	var buf bytes.Buffer
	w := NewMessageWriter(&buf)
	if err := w.Write(MsgCommand, []byte(`{"cmd":"shutdown"}`)); err != nil {
		t.Fatal(err)
	}
	if err := w.Write(MsgVideoAU, EncodeMedia(Media{CaptureNs: 5, SendNs: 9, Keyframe: true, Data: []byte{1, 2, 3}})); err != nil {
		t.Fatal(err)
	}
	m1, err := ReadMessage(&buf)
	if err != nil || m1.Type != MsgCommand || string(m1.Payload) != `{"cmd":"shutdown"}` {
		t.Fatalf("m1 = %+v, %v", m1, err)
	}
	m2, err := ReadMessage(&buf)
	if err != nil || m2.Type != MsgVideoAU {
		t.Fatalf("m2 = %+v, %v", m2, err)
	}
	media, err := DecodeMedia(m2.Payload)
	if err != nil || media.CaptureNs != 5 || media.SendNs != 9 || !media.Keyframe || !bytes.Equal(media.Data, []byte{1, 2, 3}) {
		t.Fatalf("media = %+v, %v", media, err)
	}
	if _, err := ReadMessage(&buf); err != io.EOF {
		t.Fatalf("clean end: err = %v, want io.EOF", err)
	}
}

func TestReadMessageRejectsTruncatedBody(t *testing.T) {
	var hdr [4]byte
	binary.LittleEndian.PutUint32(hdr[:], 10)
	r := bytes.NewReader(append(hdr[:], MsgCommand, 'x'))
	if _, err := ReadMessage(r); err == nil || err == io.EOF {
		t.Fatalf("err = %v, want truncation error", err)
	}
}

func TestReadMessageRejectsOversizeAndZeroLength(t *testing.T) {
	var hdr [4]byte
	binary.LittleEndian.PutUint32(hdr[:], MaxMessage+1)
	if _, err := ReadMessage(bytes.NewReader(hdr[:])); !errors.Is(err, ErrMessageTooLarge) {
		t.Fatalf("oversize: err = %v", err)
	}
	binary.LittleEndian.PutUint32(hdr[:], 0)
	if _, err := ReadMessage(bytes.NewReader(hdr[:])); err == nil {
		t.Fatal("zero length accepted")
	}
}

func TestDecodeMediaRejectsShortPayload(t *testing.T) {
	if _, err := DecodeMedia(make([]byte, 16)); err == nil {
		t.Fatal("16-byte media payload accepted")
	}
	// EncodeMedia with nil Data produces 17-byte header-only payload, which DecodeMedia must reject
	if _, err := DecodeMedia(EncodeMedia(Media{CaptureNs: 1, SendNs: 2})); err == nil {
		t.Fatal("header-only media payload accepted")
	}
}

func TestMediaTimeMapsCaptureThroughClock(t *testing.T) {
	cm := newClockMap(clockMapWindow, clockMapSlewPPM, clockMapStep)
	now := time.Unix(1000, 0)
	if got := mediaTime(cm, now, Media{CaptureNs: 1_000_000, SendNs: 41_000_000}); !got.Equal(now.Add(-40 * time.Millisecond)) {
		t.Fatalf("first message: %v, want now-40ms", got)
	}
	// Send before capture (a clock glitch in the plugin): treat the age as zero.
	if got := mediaTime(cm, now.Add(10*time.Millisecond), Media{CaptureNs: 60_000_000, SendNs: 51_000_000}); got.After(now.Add(10 * time.Millisecond)) {
		t.Fatalf("send before capture mapped into the future: %v", got)
	}
}

func TestHelloValidate(t *testing.T) {
	if err := DefaultHello().Validate(); err != nil {
		t.Fatalf("default hello invalid: %v", err)
	}
	h := DefaultHello()
	h.Audio.SampleRate = 48000
	if err := h.Validate(); err == nil {
		t.Fatal("48 kHz accepted")
	}
	h = DefaultHello()
	h.Version = 2
	if err := h.Validate(); err == nil {
		t.Fatal("version 2 accepted")
	}
	h = DefaultHello()
	h.Video.Codec = "hevc"
	if err := h.Validate(); err == nil {
		t.Fatal("hevc accepted")
	}
}
