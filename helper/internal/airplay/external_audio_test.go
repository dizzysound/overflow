package airplay

import (
	"bytes"
	"errors"
	"io"
	"math/rand"
	"testing"
	"time"
)

func pcmRamp(samples int, start byte) []byte {
	out := make([]byte, samples*4)
	for i := range out {
		out[i] = start + byte(i)
	}
	return out
}

func subscriberReader(t *testing.T, ac *AudioCapture) *externalPCMReader {
	t.Helper()
	r, ok := ac.pcmFrames.(*externalPCMReader)
	if !ok {
		t.Fatalf("pcmFrames is %T", ac.pcmFrames)
	}
	return r
}

func TestExternalAudioReframesWithPTS(t *testing.T) {
	src := NewExternalAudioSource()
	ac, err := src.Subscribe(AudioCodecALAC)
	if err != nil {
		t.Fatal(err)
	}
	r := subscriberReader(t, ac)
	t0 := time.Unix(100, 0)
	pcm := pcmRamp(1000, 0)
	if err := src.Push(t0, pcm); err != nil {
		t.Fatal(err)
	}
	frame := make([]byte, 352*4)
	pts, err := r.ReadPCMFrame(frame)
	if err != nil || !pts.Equal(t0) || !bytes.Equal(frame, pcm[:352*4]) {
		t.Fatalf("first frame pts=%v err=%v", pts, err)
	}
	pts, err = r.ReadPCMFrame(frame)
	if err != nil || !pts.Equal(t0.Add(audioSamplesDuration(352))) || !bytes.Equal(frame, pcm[352*4:704*4]) {
		t.Fatalf("second frame pts=%v err=%v", pts, err)
	}
}

func TestExternalAudioFansOutToEverySubscriber(t *testing.T) {
	src := NewExternalAudioSource()
	a, _ := src.Subscribe(AudioCodecALAC)
	b, _ := src.Subscribe(AudioCodecALAC)
	pcm := pcmRamp(352, 7)
	src.Push(time.Unix(1, 0), pcm)
	for name, ac := range map[string]*AudioCapture{"a": a, "b": b} {
		frame := make([]byte, 352*4)
		if _, err := subscriberReader(t, ac).ReadPCMFrame(frame); err != nil || !bytes.Equal(frame, pcm) {
			t.Fatalf("subscriber %s: %v", name, err)
		}
	}
}

func TestExternalAudioOverflowKeepsNewestSecond(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	t0 := time.Unix(200, 0)
	src.Push(t0, pcmRamp(66150, 0)) // 1.5 s, never read
	frame := make([]byte, 352*4)
	pts, err := subscriberReader(t, ac).ReadPCMFrame(frame)
	if err != nil || !pts.Equal(t0.Add(audioSamplesDuration(22050))) {
		t.Fatalf("pts = %v, want t0+0.5s; err=%v", pts, err)
	}
}

func TestExternalAudioRejectsPartialSampleFrames(t *testing.T) {
	src := NewExternalAudioSource()
	if err := src.Push(time.Now(), make([]byte, 6)); err == nil {
		t.Fatal("6-byte push accepted")
	}
}

func TestExternalAudioSubscribeProducesALACFrames(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	t0 := time.Unix(300, 0)
	src.Push(t0, pcmRamp(352, 0))
	buf := make([]byte, 4096)
	n, pts, err := ac.ReadFrameAt(buf)
	if err != nil || n == 0 || !pts.Equal(t0) {
		t.Fatalf("ReadFrameAt n=%d pts=%v err=%v", n, pts, err)
	}
}

func TestExternalAudioStopUnsubscribes(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	done := make(chan struct{})
	go func() { ac.Stop(); close(done) }()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		t.Fatal("Stop did not return")
	}
	if n := src.subscriberCount(); n != 0 {
		t.Fatalf("subscribers after Stop = %d", n)
	}
}

func TestExternalAudioCloseEndsReadersAndPushes(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	src.Close()
	if _, err := subscriberReader(t, ac).ReadPCMFrame(make([]byte, 1408)); err != io.EOF {
		t.Fatalf("read after close: %v", err)
	}
	if err := src.Push(time.Now(), make([]byte, 4)); !errors.Is(err, ErrExternalSourceClosed) {
		t.Fatalf("push after close: %v", err)
	}
	if _, err := src.Subscribe(AudioCodecALAC); !errors.Is(err, ErrExternalSourceClosed) {
		t.Fatalf("subscribe after close: %v", err)
	}
}

// obsChunks returns n OBS-shaped 44.1 kHz chunks: OBS ticks 1024 frames at 48 kHz,
// so each chunk holds 940 or 941 frames and starts at an exact capture time.
func obsChunks(n int) (captures []uint64, sizes []int) {
	const start = uint64(7_200_000_000_000)
	produced := 0
	for k := 0; k < n; k++ {
		end := (k + 1) * 1024 * 44100 / 48000
		sizes = append(sizes, end-produced)
		captures = append(captures, start+uint64(produced)*1_000_000_000/44100)
		produced = end
	}
	return captures, sizes
}

func readPositions(t *testing.T, r *externalPCMReader, spf int, frames int) []audioPCMFramePosition {
	t.Helper()
	out := make([]audioPCMFramePosition, 0, frames)
	dst := make([]byte, spf*4)
	for i := 0; i < frames; i++ {
		p, err := r.ReadPCMFramePosition(dst)
		if err != nil {
			t.Fatal(err)
		}
		out = append(out, p)
	}
	return out
}

// The regression this plan fixes: arrival-time jitter on each chunk's PTS must
// not move the RTP timeline (data/2026-09-28-quality-latency/16-rtp-gaps.md).
func TestExternalAudioRTPStaysContinuousUnderArrivalJitter(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	r := subscriberReader(t, ac)
	rng := rand.New(rand.NewSource(3))
	captures, sizes := obsChunks(3000) // about 64 s
	base := time.Unix(9000, 0)
	clock := newAudioRTPClock(1000)
	dst := make([]byte, 480*4)
	var last uint32
	frames := 0
	for k := range captures {
		// The PTS carries up to 1 ms of arrival jitter, as bridge.PTS did on Windows.
		jitter := time.Duration(rng.Int63n(int64(time.Millisecond)))
		pts := base.Add(time.Duration(captures[k]-captures[0]) + jitter)
		if err := src.PushCaptured(captures[k], pts, make([]byte, sizes[k]*4)); err != nil {
			t.Fatal(err)
		}
		// Read in step with the pushes, as a live session does.
		for {
			r.mu.Lock()
			ready := len(r.buf) >= len(dst)
			r.mu.Unlock()
			if !ready {
				break
			}
			p, err := r.ReadPCMFramePosition(dst)
			if err != nil {
				t.Fatal(err)
			}
			rtp, reset := clock.mapFramePosition(p, 480)
			if reset {
				t.Fatalf("frame %d: unexpected reset", frames)
			}
			if frames > 0 && rtp-last != 480 {
				t.Fatalf("frame %d: RTP step %d, want 480", frames, int64(int32(rtp-last)))
			}
			last = rtp
			frames++
		}
	}
	if frames < 5800 {
		t.Fatalf("read %d frames, want at least 5800", frames)
	}
}

func TestExternalAudioFillsShortGapWithSilence(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	r := subscriberReader(t, ac)
	t0 := time.Unix(50, 0)
	const c0 = uint64(1_000_000_000)
	src.PushCaptured(c0, t0, pcmRamp(441, 1))                                     // 0-10 ms
	src.PushCaptured(c0+20_000_000, t0.Add(20*time.Millisecond), pcmRamp(441, 9)) // 20-30 ms; 10-20 ms was dropped upstream
	got := make([]byte, 1323*4)
	p, err := r.ReadPCMFramePosition(got)
	if err != nil || p.SourceRTP != 0 || !p.PTS.Equal(t0) {
		t.Fatalf("position %+v err %v", p, err)
	}
	if !bytes.Equal(got[:441*4], pcmRamp(441, 1)) || !bytes.Equal(got[441*4:882*4], make([]byte, 441*4)) ||
		!bytes.Equal(got[882*4:], pcmRamp(441, 9)) {
		t.Fatal("want chunk A, 441 samples of silence, then chunk B at its true position")
	}
	if s := src.InputStats(); s.Filled != 441 || s.Dropped != 0 || s.Restarts != 0 {
		t.Fatalf("stats %+v", s)
	}
}

func TestExternalAudioDropsOverlap(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	r := subscriberReader(t, ac)
	t0 := time.Unix(60, 0)
	const c0 = uint64(2_000_000_000)
	a := pcmRamp(441, 1)
	b := pcmRamp(441, 50)
	src.PushCaptured(c0, t0, a)
	// B starts 100 samples before A ends: those 100 samples are already queued.
	src.PushCaptured(c0+uint64(341)*1_000_000_000/44100, t0, b)
	got := make([]byte, 782*4)
	if _, err := r.ReadPCMFramePosition(got); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got[:441*4], a) || !bytes.Equal(got[441*4:], b[100*4:]) {
		t.Fatal("want A whole, then B without its first 100 samples")
	}
	if s := src.InputStats(); s.Dropped != 100 {
		t.Fatalf("stats %+v, want 100 dropped", s)
	}
}

func TestExternalAudioDropsDuplicateChunk(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	r := subscriberReader(t, ac)
	t0 := time.Unix(61, 0)
	const c0 = uint64(3_000_000_000)
	src.PushCaptured(c0, t0, pcmRamp(441, 1))
	src.PushCaptured(c0, t0, pcmRamp(441, 99)) // the same capture again
	src.PushCaptured(c0+10_000_000, t0.Add(10*time.Millisecond), pcmRamp(441, 2))
	got := make([]byte, 882*4)
	if _, err := r.ReadPCMFramePosition(got); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(got[441*4:], pcmRamp(441, 2)) {
		t.Fatal("duplicate chunk was not dropped")
	}
}

func TestExternalAudioLongGapRestartsTimeline(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	r := subscriberReader(t, ac)
	t0 := time.Unix(70, 0)
	const c0 = uint64(4_000_000_000)
	src.PushCaptured(c0, t0, pcmRamp(352, 1))
	frame := make([]byte, 352*4)
	if p, _ := r.ReadPCMFramePosition(frame); p.SourceRTP != 0 {
		t.Fatalf("first position %d", p.SourceRTP)
	}
	// The output restarts a minute later: no minute of silence, a new segment.
	src.PushCaptured(c0+60_000_000_000, t0.Add(time.Minute), pcmRamp(352, 5))
	p, err := r.ReadPCMFramePosition(frame)
	if err != nil || p.SourceRTP != 60*44100 || !p.PTS.Equal(t0.Add(time.Minute)) || !bytes.Equal(frame, pcmRamp(352, 5)) {
		t.Fatalf("after restart: %+v err %v", p, err)
	}
	if s := src.InputStats(); s.Restarts != 1 || s.Filled != 0 {
		t.Fatalf("stats %+v", s)
	}
}

func TestExternalAudioBurstAfterStallStaysContiguous(t *testing.T) {
	src := NewExternalAudioSource()
	ac, _ := src.Subscribe(AudioCodecALAC)
	r := subscriberReader(t, ac)
	captures, sizes := obsChunks(20)
	base := time.Unix(80, 0)
	// OBS stops calling back for 3 ticks, then delivers them in a burst: the
	// captures stay contiguous, only arrival is late.
	for k := range captures {
		arrival := base.Add(time.Duration(captures[k] - captures[0]))
		if k >= 5 && k < 8 {
			arrival = base.Add(time.Duration(captures[8] - captures[0]))
		}
		src.PushCaptured(captures[k], arrival, make([]byte, sizes[k]*4))
	}
	var want uint32
	for i, p := range readPositions(t, r, 352, 50) {
		if p.SourceRTP != want {
			t.Fatalf("frame %d at %d, want %d", i, p.SourceRTP, want)
		}
		want += 352
	}
	if s := src.InputStats(); s.Filled != 0 || s.Dropped != 0 || s.Restarts != 0 {
		t.Fatalf("stats %+v", s)
	}
}

func TestExternalAudioLateSubscriberSharesPositions(t *testing.T) {
	src := NewExternalAudioSource()
	a, _ := src.Subscribe(AudioCodecALAC)
	t0 := time.Unix(90, 0)
	const c0 = uint64(5_000_000_000)
	src.PushCaptured(c0, t0, pcmRamp(704, 1))
	b, _ := src.Subscribe(AudioCodecALAC) // joins after the first chunk
	src.PushCaptured(c0+uint64(704)*1_000_000_000/44100, t0.Add(audioSamplesDuration(704)), pcmRamp(352, 3))
	pa := readPositions(t, subscriberReader(t, a), 352, 3)
	pb := readPositions(t, subscriberReader(t, b), 352, 1)
	if pb[0].SourceRTP != pa[2].SourceRTP || !pb[0].PTS.Equal(pa[2].PTS) {
		t.Fatalf("late subscriber at %d/%v, first subscriber's same frame at %d/%v",
			pb[0].SourceRTP, pb[0].PTS, pa[2].SourceRTP, pa[2].PTS)
	}
}
