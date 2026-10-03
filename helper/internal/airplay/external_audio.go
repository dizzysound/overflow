package airplay

import (
	"fmt"
	"io"
	"log"
	"sync"
	"time"
)

const (
	externalAudioBytesPerFrame = 4                                  // stereo S16LE
	externalAudioMaxBuffered   = 44100 * externalAudioBytesPerFrame // one second

	// externalAudioSnapSamples absorbs rounding: a chunk this close to the
	// expected position is contiguous. OBS's tap timestamps are exact to about
	// 1 ns on the 44.1 kHz sample grid, so rounding alone gives exact positions.
	externalAudioSnapSamples = 2
	// externalAudioMaxFill is the longest forward gap filled with silence (a
	// chunk the plugin dropped, for example). Longer gaps restart the timeline.
	externalAudioMaxFill = 44100 / 2
	// externalAudioMaxOverlap is the furthest back a chunk may start and still
	// be treated as already-queued audio; further back restarts the timeline.
	externalAudioMaxOverlap = 44100
	// externalAudioStatsEvery is how often input discontinuities are logged.
	externalAudioStatsEvery = 10 * time.Second
)

// ExternalAudioSource fans one 44.1 kHz stereo S16LE PCM stream (from the OBS
// plugin) out to every receiver session. Each subscriber has its own bounded
// buffer, so a stalled receiver never delays the others.
//
// Every chunk gets a sample position on the source's own timeline. With
// PushCaptured the position comes from the chunk's capture time on the plugin
// clock, which OBS keeps exactly on the sample grid; the receiver-facing RTP
// timeline then advances by exactly the samples sent, as OBS's own encoders,
// OwnTone, libraop and pyatv do, and arrival jitter cannot move it. A short
// forward gap becomes silence and an overlap is dropped (shairport-sync's rule
// at the receiver); anything larger restarts the timeline.
type ExternalAudioSource struct {
	mu     sync.Mutex
	subs   map[*externalPCMReader]struct{}
	closed bool

	haveOrigin bool
	origin     uint64 // capture_ns of position 0
	haveNext   bool
	next       int64 // position just after the last chunk delivered

	stats       ExternalAudioInputStats
	logged      ExternalAudioInputStats
	lastStatsAt time.Time
}

// ExternalAudioInputStats counts input discontinuities since the source started.
type ExternalAudioInputStats struct {
	Filled   uint64 // samples of silence inserted for forward gaps
	Dropped  uint64 // overlapping samples dropped
	Restarts int    // timeline restarts (long gaps, far overlaps)
}

func NewExternalAudioSource() *ExternalAudioSource {
	return &ExternalAudioSource{subs: make(map[*externalPCMReader]struct{})}
}

// Push delivers PCM whose first sample is presented at pts, contiguous with the
// previous push.
func (s *ExternalAudioSource) Push(pts time.Time, pcm []byte) error {
	return s.push(0, false, pts, pcm)
}

// PushCaptured delivers PCM whose first sample was captured at captureNs on the
// plugin's clock and is presented at pts on the helper's clock.
func (s *ExternalAudioSource) PushCaptured(captureNs uint64, pts time.Time, pcm []byte) error {
	return s.push(captureNs, true, pts, pcm)
}

func (s *ExternalAudioSource) push(captureNs uint64, captured bool, pts time.Time, pcm []byte) error {
	if len(pcm)%externalAudioBytesPerFrame != 0 {
		return fmt.Errorf("external audio: %d bytes is not a whole number of stereo S16LE frames", len(pcm))
	}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return ErrExternalSourceClosed
	}
	pos := s.next
	if captured {
		if !s.haveOrigin {
			s.haveOrigin, s.origin = true, captureNs
		}
		pos = captureToSamples(captureNs, s.origin)
	}
	if !s.haveNext {
		s.haveNext, s.next = true, pos
	}
	frames := int64(len(pcm) / externalAudioBytesPerFrame)
	switch d := pos - s.next; {
	case d >= -externalAudioSnapSamples && d <= externalAudioSnapSamples:
		pos = s.next
	case d > 0 && d <= externalAudioMaxFill:
		pcm = append(make([]byte, int(d)*externalAudioBytesPerFrame), pcm...)
		pts = pts.Add(-audioSamplesDuration(uint64(d)))
		pos = s.next
		s.stats.Filled += uint64(d)
	case d < 0 && -d < frames:
		pcm = pcm[int(-d)*externalAudioBytesPerFrame:]
		pts = pts.Add(audioSamplesDuration(uint64(-d)))
		pos = s.next
		s.stats.Dropped += uint64(-d)
	case d < 0 && -d <= externalAudioMaxOverlap:
		s.stats.Dropped += uint64(frames)
		s.logStatsLocked()
		s.mu.Unlock()
		return nil
	default:
		s.stats.Restarts++
	}
	s.next = pos + int64(len(pcm)/externalAudioBytesPerFrame)
	s.logStatsLocked()
	subs := make([]*externalPCMReader, 0, len(s.subs))
	for r := range s.subs {
		subs = append(subs, r)
	}
	s.mu.Unlock()
	for _, r := range subs {
		r.push(pts, pos, pcm)
	}
	return nil
}

// captureToSamples converts a capture time to a sample position, rounding to
// the nearest sample. A capture before the origin gives a negative position.
func captureToSamples(captureNs, origin uint64) int64 {
	if captureNs < origin {
		return -captureToSamples(origin, captureNs)
	}
	d := captureNs - origin
	whole := d / 1_000_000_000 * audioSampleRate
	return int64(whole + (d%1_000_000_000*audioSampleRate+500_000_000)/1_000_000_000)
}

// logStatsLocked logs new input discontinuities at most once per
// externalAudioStatsEvery. Callers hold s.mu.
func (s *ExternalAudioSource) logStatsLocked() {
	if s.stats == s.logged {
		return
	}
	now := time.Now()
	if !s.lastStatsAt.IsZero() && now.Sub(s.lastStatsAt) < externalAudioStatsEvery {
		return
	}
	log.Printf("[audio] input timeline: %d samples of silence filled, %d overlapping samples dropped, %d restarts",
		s.stats.Filled-s.logged.Filled, s.stats.Dropped-s.logged.Dropped, s.stats.Restarts-s.logged.Restarts)
	s.logged, s.lastStatsAt = s.stats, now
}

// InputStats returns the input discontinuity counts since the source started.
func (s *ExternalAudioSource) InputStats() ExternalAudioInputStats {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.stats
}

// Subscribe returns an AudioCapture for one receiver session. Stop unsubscribes it.
func (s *ExternalAudioSource) Subscribe(codec AudioCodec) (*AudioCapture, error) {
	if codec != AudioCodecALAC && codec != AudioCodecAACELD {
		return nil, fmt.Errorf("unsupported audio codec %d", codec)
	}
	r := newExternalPCMReader()
	ac := &AudioCapture{pcmFrames: r, codec: codec, waitCh: make(chan struct{})}
	if codec == AudioCodecAACELD {
		eld, err := newELDEncoder()
		if err != nil {
			return nil, err
		}
		ac.eld = eld
	}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		if ac.eld != nil {
			ac.eld.Close()
		}
		return nil, ErrExternalSourceClosed
	}
	s.subs[r] = struct{}{}
	s.mu.Unlock()

	var once sync.Once
	ac.cancel = func() {
		once.Do(func() {
			s.mu.Lock()
			delete(s.subs, r)
			s.mu.Unlock()
			r.close()
			close(ac.waitCh)
		})
	}
	return ac, nil
}

func (s *ExternalAudioSource) Close() {
	s.mu.Lock()
	s.closed = true
	subs := s.subs
	s.subs = make(map[*externalPCMReader]struct{})
	s.mu.Unlock()
	for r := range subs {
		r.close()
	}
}

func (s *ExternalAudioSource) subscriberCount() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.subs)
}

// externalPCMReader re-frames arbitrary PCM chunks into codec-sized frames,
// each with its sample position and presentation time. It implements
// audioPCMFrameReader and audioPCMFramePositionReader.
type externalPCMReader struct {
	mu     sync.Mutex
	cond   *sync.Cond
	buf    []byte
	have   bool
	pos    int64 // position of buf[0]
	refPos int64 // the newest chunk's first position...
	refPTS time.Time
	closed bool
}

func newExternalPCMReader() *externalPCMReader {
	r := &externalPCMReader{}
	r.cond = sync.NewCond(&r.mu)
	return r
}

// push appends a chunk the source has already placed. A position that does not
// follow the buffer (a source restart, or this reader's first chunk) starts a
// new segment at that position.
func (r *externalPCMReader) push(pts time.Time, pos int64, pcm []byte) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.closed {
		return
	}
	if !r.have || pos != r.pos+int64(len(r.buf)/externalAudioBytesPerFrame) {
		r.have = true
		r.buf = r.buf[:0]
		r.pos = pos
	}
	r.buf = append(r.buf, pcm...)
	r.refPos, r.refPTS = pos, pts
	if over := len(r.buf) - externalAudioMaxBuffered; over > 0 {
		r.buf = append(r.buf[:0], r.buf[over:]...)
		r.pos += int64(over / externalAudioBytesPerFrame)
	}
	r.cond.Broadcast()
}

func (r *externalPCMReader) ReadPCMFrame(dst []byte) (time.Time, error) {
	p, err := r.ReadPCMFramePosition(dst)
	return p.PTS, err
}

func (r *externalPCMReader) ReadPCMFramePosition(dst []byte) (audioPCMFramePosition, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for len(r.buf) < len(dst) && !r.closed {
		r.cond.Wait()
	}
	if r.closed {
		return audioPCMFramePosition{}, io.EOF
	}
	pos := r.pos
	// The newest chunk's correlation carries the clock mapping's slow changes.
	pts := r.refPTS
	if d := pos - r.refPos; d >= 0 {
		pts = pts.Add(audioSamplesDuration(uint64(d)))
	} else {
		pts = pts.Add(-audioSamplesDuration(uint64(-d)))
	}
	copy(dst, r.buf[:len(dst)])
	r.buf = append(r.buf[:0], r.buf[len(dst):]...)
	r.pos += int64(len(dst) / externalAudioBytesPerFrame)
	return audioPCMFramePosition{PTS: pts, SourceRTP: uint32(pos), HasSourceRTP: true}, nil
}

func (r *externalPCMReader) close() {
	r.mu.Lock()
	r.closed = true
	r.cond.Broadcast()
	r.mu.Unlock()
}
