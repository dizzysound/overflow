package airplay

import (
	"testing"
	"time"
)

func TestRelayBudgetForLead(t *testing.T) {
	cases := []struct{ lead, want time.Duration }{
		{40 * time.Millisecond, 67 * time.Millisecond},
		{97 * time.Millisecond, 67 * time.Millisecond},
		{100 * time.Millisecond, 70 * time.Millisecond},
		{150 * time.Millisecond, 120 * time.Millisecond},
	}
	for _, c := range cases {
		if got := RelayBudgetForLead(c.lead); got != c.want {
			t.Errorf("RelayBudgetForLead(%v) = %v, want %v", c.lead, got, c.want)
		}
	}
}
