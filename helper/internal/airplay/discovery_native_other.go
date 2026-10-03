//go:build !windows

package airplay

import "context"

// discoverNative has no backend outside Windows; DiscoverAirPlayDevices falls
// back to zeroconf silently.
func discoverNative(context.Context) ([]AirPlayDevice, error) {
	return nil, errNativeDiscoveryUnavailable
}
