package daemon

import (
	"context"
	"errors"
	"sync/atomic"
	"testing"
	"time"

	"doubletake/internal/airplay"
)

// A scan that returns before its window ends (no LAN interface: zeroconf
// returns at once; or a browse error) must not start the next scan at once.
// Without network the loop spun over a core rescanning (NUC container with
// --network none: 20.9 s of CPU in 20 s).
func TestBackgroundDiscoverWaitsOutScanThatReturnsEarly(t *testing.T) {
	for _, tc := range []struct {
		name string
		err  error
	}{
		{"no interfaces", nil},
		{"browse error", errors.New("browse: no multicast")},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var calls atomic.Int32
			prev := discoverDevices
			discoverDevices = func(context.Context) ([]airplay.AirPlayDevice, error) {
				calls.Add(1)
				return nil, tc.err
			}
			t.Cleanup(func() { discoverDevices = prev })

			d := &Daemon{deviceLastSeen: make(map[string]time.Time)}
			ctx, cancel := context.WithCancel(context.Background())
			done := make(chan struct{})
			go func() {
				d.backgroundDiscover(ctx)
				close(done)
			}()
			time.Sleep(300 * time.Millisecond)
			cancel()
			select {
			case <-done:
			case <-time.After(2 * time.Second):
				t.Fatal("backgroundDiscover did not return after cancel")
			}
			if n := calls.Load(); n != 1 {
				t.Fatalf("scans in 300 ms = %d, want 1 (one per 5 s window)", n)
			}
		})
	}
}
