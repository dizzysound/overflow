package bridge

import (
	"sort"
	"sync"
	"time"

	"doubletake/internal/airplay"
)

// DeliverySummary is one display's delivery ages over a flush window: OBS
// capture to the moment its bytes were written to the receiver.
type DeliverySummary struct {
	DeviceID string
	Frames   int
	P50Ms    int
	P99Ms    int
	Late     int // frames written after capture + lead
	LeadMs   int // effective lead at the end of the window
	// LeadTargetMs and LeadCeilingMs are the slide's target and the announced
	// ceiling; all three are equal for a fixed lead.
	LeadTargetMs  int
	LeadCeilingMs int
	// AudioLost counts audio packets the receiver asked to be resent (lost on
	// the way); AudioResent counts those still in the history and resent.
	AudioLost   int
	AudioResent int
	// AudioDropped counts audio frames the sender dropped as too late for the
	// lead (they would have played late): gaps the receiver never reports.
	AudioDropped int
	// AudioJumps counts audio frames whose RTP timestamp did not follow the
	// previous frame's, or whose source clock mapping was reset: timeline
	// discontinuities a receiver must conceal.
	AudioJumps int
}

type deliveryWindows struct {
	mu   sync.Mutex
	ages map[string][]time.Duration
	lead map[string]airplay.LeadState
	late map[string]int
	lost map[string]int
	sent map[string]int // resent
	drop map[string]int // audio frames dropped as late
	jump map[string]int // audio RTP discontinuities
}

func newDeliveryWindows() *deliveryWindows {
	return &deliveryWindows{ages: map[string][]time.Duration{}, lead: map[string]airplay.LeadState{}, late: map[string]int{},
		lost: map[string]int{}, sent: map[string]int{}, drop: map[string]int{}, jump: map[string]int{}}
}

// addDropped records one audio frame dropped as too late for deviceID's lead.
func (w *deliveryWindows) addDropped(deviceID string) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.drop[deviceID]++
}

// addJump records one audio RTP timeline discontinuity for deviceID.
func (w *deliveryWindows) addJump(deviceID string) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.jump[deviceID]++
}

// addResend records one receiver retransmit request for deviceID.
func (w *deliveryWindows) addResend(deviceID string, requested, resent int) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.lost[deviceID] += requested
	w.sent[deviceID] += resent
}

func (w *deliveryWindows) add(deviceID string, lead airplay.LeadState, age time.Duration) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.ages[deviceID] = append(w.ages[deviceID], age)
	w.lead[deviceID] = lead
	if lead.Effective > 0 && age > lead.Effective {
		w.late[deviceID]++
	}
}

func percentileMs(sorted []time.Duration, p int) int {
	idx := (len(sorted)*p+99)/100 - 1
	if idx < 0 {
		idx = 0
	}
	return int(sorted[idx] / time.Millisecond)
}

// flush returns one summary per display with frames, sorted by device ID, and
// resets the windows.
func (w *deliveryWindows) flush() []DeliverySummary {
	w.mu.Lock()
	defer w.mu.Unlock()
	var out []DeliverySummary
	for id, ages := range w.ages {
		if len(ages) == 0 {
			continue
		}
		sorted := append([]time.Duration(nil), ages...)
		sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
		out = append(out, DeliverySummary{
			DeviceID: id, Frames: len(sorted),
			P50Ms: percentileMs(sorted, 50), P99Ms: percentileMs(sorted, 99),
			Late: w.late[id], LeadMs: int(w.lead[id].Effective / time.Millisecond),
			LeadTargetMs: int(w.lead[id].Target / time.Millisecond), LeadCeilingMs: int(w.lead[id].Ceiling / time.Millisecond),
			AudioLost: w.lost[id], AudioResent: w.sent[id], AudioDropped: w.drop[id],
			AudioJumps: w.jump[id],
		})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].DeviceID < out[j].DeviceID })
	w.ages, w.late = map[string][]time.Duration{}, map[string]int{}
	w.lost, w.sent, w.drop, w.jump = map[string]int{}, map[string]int{}, map[string]int{}, map[string]int{}
	return out
}
