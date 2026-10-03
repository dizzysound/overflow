package daemon

import (
	"errors"
	"fmt"
	"reflect"
	"testing"

	"doubletake/internal/airplay"
)

func TestRetryMirrorSetupAfterLateDigestChallenge(t *testing.T) {
	var events []string
	var password string
	retryErr := errors.New("retry result")

	err := retryMirrorSetupAfterDigestChallenge(
		fmt.Errorf("audio stream SETUP: %w", airplay.ErrCredentialsRequired),
		func() (string, error) {
			events = append(events, "wait")
			return "configured code", nil
		},
		func(value string) {
			events = append(events, "password")
			password = value
		},
		func() error {
			events = append(events, "retry")
			return retryErr
		},
	)

	if !errors.Is(err, retryErr) {
		t.Fatalf("retry error = %v, want %v", err, retryErr)
	}
	if password != "configured code" {
		t.Fatalf("password = %q, want configured code", password)
	}
	wantEvents := []string{"wait", "password", "retry"}
	if !reflect.DeepEqual(events, wantEvents) {
		t.Fatalf("events = %v, want %v", events, wantEvents)
	}
}

func TestRetryMirrorSetupIgnoresOtherSetupErrors(t *testing.T) {
	setupErr := errors.New("ordinary setup failure")
	called := false
	err := retryMirrorSetupAfterDigestChallenge(
		setupErr,
		func() (string, error) { called = true; return "", nil },
		func(string) { called = true },
		func() error { called = true; return nil },
	)
	if !errors.Is(err, setupErr) {
		t.Fatalf("error = %v, want %v", err, setupErr)
	}
	if called {
		t.Fatal("non-credential setup failure entered credential retry flow")
	}
}

func TestRetryMirrorSetupStopsWhenCredentialWaitFails(t *testing.T) {
	waitErr := errors.New("prompt cancelled")
	var events []string
	retries := 0
	err := retryMirrorSetupAfterDigestChallenge(
		airplay.ErrCredentialsRequired,
		func() (string, error) {
			events = append(events, "wait")
			return "code", waitErr
		},
		func(string) { events = append(events, "password") },
		func() error {
			retries++
			return nil
		},
	)
	if !errors.Is(err, waitErr) {
		t.Fatalf("error = %v, want wrapped %v", err, waitErr)
	}
	if retries != 0 {
		t.Fatalf("retry count = %d, want 0", retries)
	}
	if want := []string{"wait"}; !reflect.DeepEqual(events, want) {
		t.Fatalf("events = %v, want %v", events, want)
	}
}

func TestRetryMirrorSetupPromptsAndRetriesOnlyOnce(t *testing.T) {
	prompts := 0
	retries := 0
	err := retryMirrorSetupAfterDigestChallenge(
		airplay.ErrCredentialsRequired,
		func() (string, error) { prompts++; return "wrong code", nil },
		func(string) {},
		func() error { retries++; return airplay.ErrCredentialsRequired },
	)
	if !errors.Is(err, airplay.ErrCredentialsRequired) {
		t.Fatalf("error = %v, want credentials required", err)
	}
	if prompts != 1 || retries != 1 {
		t.Fatalf("prompts=%d retries=%d, want 1 each", prompts, retries)
	}
}
