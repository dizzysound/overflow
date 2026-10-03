//go:build !cgo || !fdk_aac

package airplay

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sync"
	"time"
)

// This build encodes AAC-ELD in a separate program, eld-encoder, built from
// eld-encoder/ against the Fraunhofer FDK AAC library. Nothing from fdk-aac is
// linked into this binary: the helper starts the encoder and exchanges frames
// over its stdin and stdout. When the program is absent, newELDEncoder returns
// ErrAACELDUnavailable and AAC-ELD-only receivers get video without audio.
//
// Protocol (see eld-encoder/README.md): the encoder writes "ELD1" and a u32
// little-endian frame length (480). For each 1920-byte S16LE stereo PCM frame
// written to its stdin, it answers with a u32 LE length and that many bytes of
// one raw AAC-ELD access unit; zero means no output for that frame.

const (
	eldEncoderMagic = "ELD1"
	eldFrameLength  = 480
	// eldMaxAccessUnit bounds a single reply. At 128 kbit/s an AU is about
	// 174 bytes; anything this large means the stream is corrupt.
	eldMaxAccessUnit = 64 * 1024
)

var (
	eldEncoderPathMu       sync.Mutex
	eldEncoderPathOverride string

	// eldEncoderTimeout bounds the handshake and each frame, so a hung
	// encoder cannot wedge the audio loop. Tests shorten it.
	eldEncoderTimeout = 2 * time.Second
	// eldEncoderCloseGrace is how long Close waits for a clean exit after
	// closing stdin before it kills the encoder.
	eldEncoderCloseGrace = time.Second
	// eldEncoderLogf receives the encoder's stderr lines.
	eldEncoderLogf = log.Printf
)

// SetELDEncoderPath sets the eld-encoder program to run. An empty path selects
// the default: eld-encoder (eld-encoder.exe on Windows) in the directory of
// the running executable.
func SetELDEncoderPath(path string) {
	eldEncoderPathMu.Lock()
	eldEncoderPathOverride = path
	eldEncoderPathMu.Unlock()
}

func eldEncoderBinaryName() string {
	if runtime.GOOS == "windows" {
		return "eld-encoder.exe"
	}
	return "eld-encoder"
}

// eldEncoderPath returns the configured encoder path, or the default next to
// the running executable.
func eldEncoderPath() (string, error) {
	eldEncoderPathMu.Lock()
	path := eldEncoderPathOverride
	eldEncoderPathMu.Unlock()
	if path != "" {
		return path, nil
	}
	exe, err := os.Executable()
	if err != nil {
		return "", fmt.Errorf("locate the helper executable: %w", err)
	}
	if resolved, err := filepath.EvalSymlinks(exe); err == nil {
		exe = resolved
	}
	return filepath.Join(filepath.Dir(exe), eldEncoderBinaryName()), nil
}

type eldEncoder struct {
	cmd      *exec.Cmd
	stdin    *os.File // our write end of the encoder's stdin
	stdout   *os.File // our read end of the encoder's stdout
	reader   *bufio.Reader
	frameLen int
	stderrW  *io.PipeWriter

	// exited is closed once cmd.Wait has returned, so the child is reaped.
	exited chan struct{}

	mu     sync.Mutex // serializes Encode
	failed error      // set after any encode failure; later calls return it

	closeOnce sync.Once
}

func newELDEncoder() (*eldEncoder, error) {
	path, err := eldEncoderPath()
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrAACELDUnavailable, err)
	}
	info, err := os.Stat(path)
	if err != nil || info.IsDir() {
		return nil, fmt.Errorf("%w: eld-encoder not found at %s (ship it next to the helper or pass -eld-encoder)", ErrAACELDUnavailable, path)
	}

	// Explicit os.Pipe ends, rather than StdinPipe/StdoutPipe, so a goroutine
	// can call Wait as soon as the child exits (reaping it) without racing
	// our reads, which exec forbids for its own pipes.
	childStdin, parentStdin, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("%w: stdin pipe: %v", ErrAACELDUnavailable, err)
	}
	parentStdout, childStdout, err := os.Pipe()
	if err != nil {
		childStdin.Close()
		parentStdin.Close()
		return nil, fmt.Errorf("%w: stdout pipe: %v", ErrAACELDUnavailable, err)
	}
	stderrR, stderrW := io.Pipe()

	cmd := exec.Command(path)
	cmd.Stdin = childStdin
	cmd.Stdout = childStdout
	cmd.Stderr = stderrW
	cmd.WaitDelay = time.Second
	configureELDEncoderCommand(cmd)
	if err := cmd.Start(); err != nil {
		childStdin.Close()
		parentStdin.Close()
		childStdout.Close()
		parentStdout.Close()
		stderrW.Close()
		return nil, fmt.Errorf("%w: start %s: %v", ErrAACELDUnavailable, path, err)
	}
	childStdin.Close()
	childStdout.Close()
	go forwardELDEncoderStderr(stderrR)

	e := &eldEncoder{
		cmd:     cmd,
		stdin:   parentStdin,
		stdout:  parentStdout,
		reader:  bufio.NewReaderSize(parentStdout, 4096),
		stderrW: stderrW,
		exited:  make(chan struct{}),
	}
	go func() {
		_ = cmd.Wait()
		stderrW.Close()
		close(e.exited)
	}()

	frameLen, err := e.handshake()
	if err != nil {
		e.Close()
		return nil, fmt.Errorf("%w: %s: %v", ErrAACELDUnavailable, path, err)
	}
	e.frameLen = frameLen
	return e, nil
}

func forwardELDEncoderStderr(r io.Reader) {
	scanner := bufio.NewScanner(r)
	for scanner.Scan() {
		eldEncoderLogf("[ELD-ENCODER] %s", scanner.Text())
	}
	// Keep draining so a chatty encoder never blocks on a full stderr pipe.
	_, _ = io.Copy(io.Discard, r)
}

func (e *eldEncoder) handshake() (int, error) {
	var frameLen int
	err := e.withTimeout("handshake", func() error {
		var header [8]byte
		if _, err := io.ReadFull(e.reader, header[:]); err != nil {
			return fmt.Errorf("read handshake: %w", err)
		}
		if string(header[:4]) != eldEncoderMagic {
			return fmt.Errorf("handshake magic %q, want %q", header[:4], eldEncoderMagic)
		}
		frameLen = int(binary.LittleEndian.Uint32(header[4:]))
		if frameLen != eldFrameLength {
			return fmt.Errorf("encoder selected %d samples per frame, want %d", frameLen, eldFrameLength)
		}
		return nil
	})
	return frameLen, err
}

// withTimeout runs op, which does blocking pipe I/O, and kills the encoder if
// it takes longer than eldEncoderTimeout. It returns only after op has
// finished, so op never touches caller memory after withTimeout returns.
func (e *eldEncoder) withTimeout(what string, op func() error) error {
	done := make(chan error, 1)
	go func() { done <- op() }()
	timer := time.NewTimer(eldEncoderTimeout)
	defer timer.Stop()
	select {
	case err := <-done:
		return err
	case <-e.exited:
		// The encoder died; op will see EOF or a broken pipe promptly.
		err := <-done
		if err == nil {
			return nil
		}
		return fmt.Errorf("%s: encoder exited: %w", what, err)
	case <-timer.C:
		e.kill()
		<-done
		return fmt.Errorf("%s: eld-encoder did not respond within %v", what, eldEncoderTimeout)
	}
}

// Encode sends one 480-sample S16LE stereo frame and returns the size of the
// raw AAC-ELD access unit written to out. It matches the cgo encoder: a frame
// of the wrong size or an empty out is an error, a frame with no output returns
// (0, nil), and an access unit larger than out is an error. Any failure is
// permanent for this encoder, which ends the session's audio.
func (e *eldEncoder) Encode(pcm, out []byte) (int, error) {
	wantPCM := e.frameLen * 2 * 2 // samples * stereo * S16LE
	if len(pcm) != wantPCM {
		return 0, fmt.Errorf("AAC-ELD PCM frame is %d bytes, want %d", len(pcm), wantPCM)
	}
	if len(out) == 0 {
		return 0, fmt.Errorf("AAC-ELD output buffer is empty")
	}
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.failed != nil {
		return 0, e.failed
	}
	var n int
	err := e.withTimeout("encode AAC-ELD", func() error {
		if _, err := e.stdin.Write(pcm); err != nil {
			return fmt.Errorf("write PCM: %w", err)
		}
		var size [4]byte
		if _, err := io.ReadFull(e.reader, size[:]); err != nil {
			return fmt.Errorf("read access unit length: %w", err)
		}
		n = int(binary.LittleEndian.Uint32(size[:]))
		if n > eldMaxAccessUnit {
			return fmt.Errorf("access unit length %d exceeds %d", n, eldMaxAccessUnit)
		}
		if n > len(out) {
			return fmt.Errorf("AAC-ELD encoder returned invalid size %d for a %d-byte buffer", n, len(out))
		}
		if _, err := io.ReadFull(e.reader, out[:n]); err != nil {
			return fmt.Errorf("read access unit: %w", err)
		}
		return nil
	})
	if err != nil {
		e.failed = err
		e.kill()
		return 0, err
	}
	return n, nil
}

func (e *eldEncoder) kill() {
	if e.cmd.Process != nil {
		_ = e.cmd.Process.Kill()
	}
}

// Close ends the encoder: it closes stdin (a clean EOF), waits briefly for the
// process to exit, kills it otherwise, and always waits until it is reaped.
func (e *eldEncoder) Close() {
	if e == nil || e.cmd == nil {
		return
	}
	e.closeOnce.Do(func() {
		e.stdin.Close()
		select {
		case <-e.exited:
		case <-time.After(eldEncoderCloseGrace):
			e.kill()
			<-e.exited
		}
		e.stdout.Close()
	})
}
