//go:build !cgo || !fdk_aac

package airplay

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeELDEncoderModeEnv makes the test binary act as eld-encoder (the standard
// re-exec pattern): TestMain sees it and runs fakeELDEncoder instead of tests.
const (
	fakeELDEncoderModeEnv = "AIRPLAY_FAKE_ELD_ENCODER_MODE"
	fakeELDEncoderLogEnv  = "AIRPLAY_FAKE_ELD_ENCODER_LOG"
	fakeELDAUSize         = 64
)

func TestMain(m *testing.M) {
	if mode := os.Getenv(fakeELDEncoderModeEnv); mode != "" {
		os.Exit(fakeELDEncoder(mode))
	}
	os.Exit(m.Run())
}

// fakeELDEncoder speaks the eld-encoder protocol. Modes:
//
//	echo        ELD1 + 480, then one fakeELDAUSize-byte AU per frame whose
//	            bytes repeat the frame's first PCM byte; a frame starting with
//	            0xEE gets a zero-length AU
//	badmagic    writes "NOPE" + 480
//	badframe    writes ELD1 + 512
//	hang        handshake, then reads frames and never answers
//	ignoreeof   handshake, then sleeps forever even after stdin closes
//	oversize    handshake, then answers each frame with a 70000-byte AU
func fakeELDEncoder(mode string) int {
	var logFile *os.File
	if path := os.Getenv(fakeELDEncoderLogEnv); path != "" {
		f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
		if err == nil {
			logFile = f
			defer f.Close()
		}
	}
	header := make([]byte, 8)
	copy(header, "ELD1")
	binary.LittleEndian.PutUint32(header[4:], 480)
	switch mode {
	case "badmagic":
		copy(header, "NOPE")
	case "badframe":
		binary.LittleEndian.PutUint32(header[4:], 512)
	}
	os.Stdout.Write(header)
	os.Stderr.WriteString("fake encoder ready\n")
	if mode == "badmagic" || mode == "badframe" {
		return 0
	}
	if mode == "ignoreeof" {
		// Not select{}: with no other goroutines the runtime reports a
		// deadlock and exits, which would defeat the point of this mode.
		for {
			time.Sleep(time.Hour)
		}
	}
	pcm := make([]byte, 480*2*2)
	for {
		if _, err := io.ReadFull(os.Stdin, pcm); err != nil {
			if errors.Is(err, io.EOF) {
				return 0
			}
			os.Stderr.WriteString("fake encoder: short frame\n")
			return 1
		}
		if logFile != nil {
			logFile.Write([]byte{'.'})
		}
		switch mode {
		case "hang":
			continue
		case "oversize":
			reply := make([]byte, 4+70000)
			binary.LittleEndian.PutUint32(reply, 70000)
			os.Stdout.Write(reply)
			continue
		}
		size := fakeELDAUSize
		if pcm[0] == 0xEE {
			size = 0
		}
		reply := make([]byte, 4+size)
		binary.LittleEndian.PutUint32(reply, uint32(size))
		for i := 4; i < len(reply); i++ {
			reply[i] = pcm[0]
		}
		os.Stdout.Write(reply)
	}
}

// useFakeELDEncoder points newELDEncoder at this test binary in the given mode.
func useFakeELDEncoder(t *testing.T, mode string) {
	t.Helper()
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv(fakeELDEncoderModeEnv, mode)
	// A -race child otherwise sleeps 1 s at exit, which Close would treat as
	// an encoder ignoring EOF.
	t.Setenv("GORACE", "atexit_sleep_ms=0")
	withELDEncoderPath(t, self)
}

func withELDEncoderPath(t *testing.T, path string) {
	t.Helper()
	SetELDEncoderPath(path)
	t.Cleanup(func() { SetELDEncoderPath("") })
}

func withELDEncoderTimeout(t *testing.T, d time.Duration) {
	t.Helper()
	previous := eldEncoderTimeout
	eldEncoderTimeout = d
	t.Cleanup(func() { eldEncoderTimeout = previous })
}

type lockedLogBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *lockedLogBuffer) printf(format string, args ...any) {
	b.mu.Lock()
	defer b.mu.Unlock()
	fmt.Fprintf(&b.buf, format+"\n", args...)
}

func (b *lockedLogBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}

func withELDEncoderLog(t *testing.T, logs *lockedLogBuffer) {
	t.Helper()
	previous := eldEncoderLogf
	eldEncoderLogf = logs.printf
	t.Cleanup(func() { eldEncoderLogf = previous })
}

func eldPCMFrame(first byte) []byte {
	pcm := make([]byte, 480*2*2)
	pcm[0] = first
	return pcm
}

func TestELDProcessHandshakeAndFrameRoundTrip(t *testing.T) {
	useFakeELDEncoder(t, "echo")
	encoder, err := newELDEncoder()
	if err != nil {
		t.Fatalf("newELDEncoder: %v", err)
	}
	defer encoder.Close()

	out := make([]byte, 8192)
	for _, first := range []byte{0x11, 0x22, 0x33} {
		n, err := encoder.Encode(eldPCMFrame(first), out)
		if err != nil {
			t.Fatalf("Encode: %v", err)
		}
		if want := bytes.Repeat([]byte{first}, fakeELDAUSize); !bytes.Equal(out[:n], want) {
			t.Fatalf("Encode returned %d bytes %x, want %x", n, out[:min(n, 8)], want[:8])
		}
	}
}

func TestELDProcessZeroLengthAccessUnitIsEmptyOutput(t *testing.T) {
	useFakeELDEncoder(t, "echo")
	encoder, err := newELDEncoder()
	if err != nil {
		t.Fatalf("newELDEncoder: %v", err)
	}
	defer encoder.Close()

	out := make([]byte, 8192)
	n, err := encoder.Encode(eldPCMFrame(0xEE), out)
	if err != nil || n != 0 {
		t.Fatalf("Encode(zero-length AU) = %d, %v; want 0, nil (the cgo empty-output result)", n, err)
	}
	// The stream stays in sync after an empty frame.
	n, err = encoder.Encode(eldPCMFrame(0x44), out)
	if err != nil || n != fakeELDAUSize || out[0] != 0x44 {
		t.Fatalf("Encode after empty AU = %d, %v, first byte %x", n, err, out[0])
	}
}

func TestELDProcessRejectsWrongPCMSizeAndEmptyOutputLikeCgo(t *testing.T) {
	useFakeELDEncoder(t, "echo")
	encoder, err := newELDEncoder()
	if err != nil {
		t.Fatalf("newELDEncoder: %v", err)
	}
	defer encoder.Close()
	if _, err := encoder.Encode(make([]byte, 100), make([]byte, 8192)); err == nil {
		t.Fatal("Encode accepted a 100-byte PCM frame")
	}
	if _, err := encoder.Encode(eldPCMFrame(1), nil); err == nil {
		t.Fatal("Encode accepted an empty output buffer")
	}
}

func TestELDProcessAccessUnitLargerThanOutputIsAnError(t *testing.T) {
	useFakeELDEncoder(t, "echo")
	encoder, err := newELDEncoder()
	if err != nil {
		t.Fatalf("newELDEncoder: %v", err)
	}
	defer encoder.Close()
	if n, err := encoder.Encode(eldPCMFrame(1), make([]byte, 16)); err == nil {
		t.Fatalf("Encode into a 16-byte buffer returned %d bytes and no error", n)
	}
}

func TestELDProcessOversizeAccessUnitIsAnError(t *testing.T) {
	useFakeELDEncoder(t, "oversize")
	encoder, err := newELDEncoder()
	if err != nil {
		t.Fatalf("newELDEncoder: %v", err)
	}
	defer encoder.Close()
	if _, err := encoder.Encode(eldPCMFrame(1), make([]byte, 8192)); err == nil {
		t.Fatal("Encode accepted a 70000-byte access unit")
	}
}

func TestELDProcessWrongMagicIsUnavailable(t *testing.T) {
	for _, mode := range []string{"badmagic", "badframe"} {
		t.Run(mode, func(t *testing.T) {
			useFakeELDEncoder(t, mode)
			encoder, err := newELDEncoder()
			if err == nil {
				encoder.Close()
				t.Fatal("newELDEncoder accepted a bad handshake")
			}
			if !errors.Is(err, ErrAACELDUnavailable) {
				t.Fatalf("error = %v, want ErrAACELDUnavailable", err)
			}
		})
	}
}

func TestELDProcessMissingBinaryIsUnavailable(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "no-such-eld-encoder")
	withELDEncoderPath(t, missing)
	encoder, err := newELDEncoder()
	if err == nil {
		encoder.Close()
		t.Fatal("newELDEncoder succeeded with a missing binary")
	}
	if !errors.Is(err, ErrAACELDUnavailable) {
		t.Fatalf("error = %v, want ErrAACELDUnavailable", err)
	}
	if !strings.Contains(err.Error(), missing) {
		t.Fatalf("error %q does not name the expected path %s", err, missing)
	}
}

func TestELDProcessDefaultPathIsNextToExecutable(t *testing.T) {
	SetELDEncoderPath("")
	got, err := eldEncoderPath()
	if err != nil {
		t.Fatal(err)
	}
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	name := "eld-encoder"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	if filepath.Base(got) != name {
		t.Fatalf("default path %s, want basename %s", got, name)
	}
	selfDir, _ := filepath.EvalSymlinks(filepath.Dir(self))
	gotDir, _ := filepath.EvalSymlinks(filepath.Dir(got))
	if selfDir != gotDir {
		t.Fatalf("default path dir %s, want the executable's dir %s", gotDir, selfDir)
	}
	// Nothing is installed beside the test binary, so the probe must fail.
	if _, err := newELDEncoder(); !errors.Is(err, ErrAACELDUnavailable) {
		t.Fatalf("newELDEncoder with no binary beside the executable = %v, want ErrAACELDUnavailable", err)
	}
}

func TestELDProcessHungEncoderTimesOut(t *testing.T) {
	useFakeELDEncoder(t, "hang")
	withELDEncoderTimeout(t, 200*time.Millisecond)
	encoder, err := newELDEncoder()
	if err != nil {
		t.Fatalf("newELDEncoder: %v", err)
	}
	defer encoder.Close()

	start := time.Now()
	_, err = encoder.Encode(eldPCMFrame(1), make([]byte, 8192))
	if err == nil {
		t.Fatal("Encode returned no error from a hung encoder")
	}
	if elapsed := time.Since(start); elapsed > 2*time.Second {
		t.Fatalf("Encode took %v to time out", elapsed)
	}
	// A timed-out encoder stays failed; later frames error immediately.
	if _, err := encoder.Encode(eldPCMFrame(1), make([]byte, 8192)); err == nil {
		t.Fatal("Encode succeeded after a timeout")
	}
	assertELDProcessGone(t, encoder)
}

func TestELDProcessCloseLeavesNoProcess(t *testing.T) {
	for _, mode := range []string{"echo", "ignoreeof"} {
		t.Run(mode, func(t *testing.T) {
			useFakeELDEncoder(t, mode)
			encoder, err := newELDEncoder()
			if err != nil {
				t.Fatalf("newELDEncoder: %v", err)
			}
			start := time.Now()
			encoder.Close()
			elapsed := time.Since(start)
			if elapsed > 3*time.Second {
				t.Fatalf("Close took %v", elapsed)
			}
			if mode == "echo" && elapsed >= eldEncoderCloseGrace {
				t.Fatalf("Close took %v; the encoder should exit on stdin EOF without a kill", elapsed)
			}
			if mode == "ignoreeof" && elapsed < eldEncoderCloseGrace {
				t.Fatalf("Close returned after %v; the encoder ignoring EOF should have needed a kill after %v", elapsed, eldEncoderCloseGrace)
			}
			assertELDProcessGone(t, encoder)
			encoder.Close() // idempotent
		})
	}
}

func assertELDProcessGone(t *testing.T, encoder *eldEncoder) {
	t.Helper()
	encoder.Close()
	if encoder.cmd.ProcessState == nil {
		t.Fatal("encoder process was not waited for")
	}
	if err := eldPIDReaped(encoder.cmd.Process.Pid); err != nil {
		t.Fatal(err)
	}
}

func TestELDProcessForwardsStderr(t *testing.T) {
	useFakeELDEncoder(t, "echo")
	var logs lockedLogBuffer
	withELDEncoderLog(t, &logs)
	encoder, err := newELDEncoder()
	if err != nil {
		t.Fatalf("newELDEncoder: %v", err)
	}
	encoder.Close()
	if got := logs.String(); !strings.Contains(got, "[ELD-ENCODER] fake encoder ready") {
		t.Fatalf("stderr not forwarded with prefix; got %q", got)
	}
}

// TestELDProcessNativeEncoderEncodesTone runs the real eld-encoder built by
// eld-encoder/build.sh native. Set ELD_ENCODER_TEST_BINARY to its path.
func TestELDProcessNativeEncoderEncodesTone(t *testing.T) {
	path := os.Getenv("ELD_ENCODER_TEST_BINARY")
	if path == "" {
		t.Skip("ELD_ENCODER_TEST_BINARY is not set")
	}
	withELDEncoderPath(t, path)
	encoder, err := newELDEncoder()
	if err != nil {
		t.Fatalf("newELDEncoder: %v", err)
	}
	defer encoder.Close()

	const frames = 100
	out := make([]byte, 8192)
	nonEmpty, total := 0, 0
	sample := 0
	for f := 0; f < frames; f++ {
		pcm := make([]byte, 480*2*2)
		for i := 0; i < 480; i++ {
			v := int16(8000 * math.Sin(2*math.Pi*440*float64(sample)/44100))
			sample++
			binary.LittleEndian.PutUint16(pcm[i*4:], uint16(v))
			binary.LittleEndian.PutUint16(pcm[i*4+2:], uint16(v))
		}
		n, err := encoder.Encode(pcm, out)
		if err != nil {
			t.Fatalf("frame %d: %v", f, err)
		}
		if n > 0 {
			nonEmpty++
			total += n
			if bytes.HasPrefix(out[:n], []byte{0xff, 0xf1}) || bytes.HasPrefix(out[:n], []byte{0xff, 0xf9}) {
				t.Fatalf("frame %d carries an ADTS header", f)
			}
		}
	}
	t.Logf("%d/%d non-empty AUs, mean %d bytes", nonEmpty, frames, total/max(nonEmpty, 1))
	if nonEmpty < frames*9/10 {
		t.Fatalf("only %d of %d frames produced output", nonEmpty, frames)
	}
	// 128 kbit/s at 480 samples per frame is about 174 bytes per AU.
	if mean := total / nonEmpty; mean < 100 || mean > 300 {
		t.Fatalf("mean AU size %d bytes, want about 174", mean)
	}
}

// TestAACELDOnlyReceiverStreamsELDAudioThroughEncoderProcess is the sibling of
// TestAACELDOnlyReceiverStreamsVideoOnlyWhenEncoderUnavailable: the same
// AAC-ELD-only legacy receiver (which rejects an ALAC descriptor), but with an
// encoder binary available. The real probe must find it, SETUP must negotiate
// AAC-ELD (ct=8), and audio packets must reach the receiver carrying the
// encoder process's output.
func TestAACELDOnlyReceiverStreamsELDAudioThroughEncoderProcess(t *testing.T) {
	useFakeELDEncoder(t, "echo")
	frameLog := filepath.Join(t.TempDir(), "frames")
	t.Setenv(fakeELDEncoderLogEnv, frameLog)

	server, session := setupLegacyAACELDOnlyMirror(t, StreamConfig{})
	defer session.Close()
	if err := session.AudioUnavailable(); err != nil {
		t.Fatalf("AudioUnavailable() = %v, want nil with an encoder available", err)
	}
	if !session.HasAudio() || session.AudioCodec() != AudioCodecAACELD {
		t.Fatalf("HasAudio=%t codec=%d; want an AAC-ELD audio stream", session.HasAudio(), session.AudioCodec())
	}
	stream := session.AudioStream()
	if stream.ct != byte(AudioCodecAACELD) || stream.spf != 480 {
		t.Fatalf("audio stream ct=%d spf=%d, want ct=8 spf=480", stream.ct, stream.spf)
	}

	source := NewExternalAudioSource()
	defer source.Close()
	capture, err := source.Subscribe(AudioCodecAACELD)
	if err != nil {
		t.Fatalf("subscribe AAC-ELD: %v", err)
	}
	defer capture.Stop()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- session.StreamAudio(ctx, capture, stream) }()

	// Feed 10 ms of PCM every 10 ms, stamped with the current time.
	go func() {
		ticker := time.NewTicker(10 * time.Millisecond)
		defer ticker.Stop()
		chunk := make([]byte, 441*4)
		for i := range chunk {
			chunk[i] = 0x10
		}
		for {
			select {
			case <-ctx.Done():
				return
			case now := <-ticker.C:
				_ = source.Push(now, chunk)
			}
		}
	}()

	frame := session.streamCipher(receiverTestAVCC([]byte{0x65, 0x80}))
	timestamp, timeline := session.frameTimeNow()
	if err := session.sendFrame(frame, true, timestamp, timeline); err != nil {
		t.Fatalf("send video frame: %v", err)
	}
	close(session.firstFrameSent)

	encodedFrames := func() int {
		data, _ := os.ReadFile(frameLog)
		return len(data)
	}
	deadline := time.Now().Add(5 * time.Second)
	for (server.Stats().AudioPackets < 20 || encodedFrames() < 20) && time.Now().Before(deadline) {
		time.Sleep(10 * time.Millisecond)
	}
	stats := server.Stats()
	// The daemon stops a session's audio the same way: cancel, then Stop the
	// capture to unblock a pending PCM read.
	cancel()
	capture.Stop()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("StreamAudio did not stop")
	}
	if stats.AudioPackets < 20 {
		t.Fatalf("receiver got %d audio packets; want AAC-ELD audio: %+v", stats.AudioPackets, stats)
	}
	if n := encodedFrames(); n < 20 {
		t.Fatalf("encoder process saw %d frames; audio did not go through eld-encoder", n)
	}
}
