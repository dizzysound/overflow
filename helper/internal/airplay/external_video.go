package airplay

import (
	"context"
	"errors"
	"io"
	"sync"
)

// ErrExternalSourceClosed is returned when media is pushed after Close.
var ErrExternalSourceClosed = errors.New("external source closed")

// ExternalVideoSource feeds encoder output produced outside doubletake (the
// OBS plugin) into the mirror pipeline through the videoAccessUnitReader seam.
// Push never blocks. When the queue is full the frame is dropped and later
// frames are discarded until the next IDR, so every receiver resumes on a
// decodable picture instead of smearing.
type ExternalVideoSource struct {
	frames    chan VideoAccessUnit
	done      chan struct{}
	closeOnce sync.Once

	mu      sync.Mutex
	needIDR bool
	dropped uint64
}

func NewExternalVideoSource(queueFrames int) *ExternalVideoSource {
	if queueFrames <= 0 {
		queueFrames = 60
	}
	return &ExternalVideoSource{
		frames: make(chan VideoAccessUnit, queueFrames),
		done:   make(chan struct{}),
	}
}

func (s *ExternalVideoSource) Push(au VideoAccessUnit) error {
	select {
	case <-s.done:
		return ErrExternalSourceClosed
	default:
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.needIDR {
		if !externalAUHasIDR(au.AnnexB) {
			s.dropped++
			return nil
		}
		s.needIDR = false
	}
	select {
	case s.frames <- au:
	default:
		s.dropped++
		s.needIDR = true
	}
	return nil
}

func (s *ExternalVideoSource) ReadVideoAccessUnit() (VideoAccessUnit, error) {
	select {
	case au := <-s.frames:
		return au, nil
	case <-s.done:
		return VideoAccessUnit{}, io.EOF
	}
}

// Capture wraps the source as a ScreenCapture for NewBroadcastCapture*. Stopping
// the capture does not close the source; Close does.
func (s *ExternalVideoSource) Capture() *ScreenCapture {
	ctx, cancel := context.WithCancel(context.Background())
	waitCh := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
		case <-s.done:
		}
		close(waitCh)
	}()
	return &ScreenCapture{frames: s, waitCh: waitCh, cancel: cancel}
}

func (s *ExternalVideoSource) Dropped() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.dropped
}

func (s *ExternalVideoSource) Close() {
	s.closeOnce.Do(func() { close(s.done) })
}

// externalAUHasIDR reports whether an Annex-B access unit contains an IDR slice
// (NAL type 5).
func externalAUHasIDR(data []byte) bool {
	for i := 0; i+3 < len(data); i++ {
		if data[i] == 0 && data[i+1] == 0 && data[i+2] == 1 {
			if data[i+3]&0x1f == 5 {
				return true
			}
			i += 2
		}
	}
	return false
}
