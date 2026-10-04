.PHONY: all build doubletake doubletake-ctl doubletake-test-receiver doubletake-release doubletake-ctl-release doubletake-test-receiver-release manpages-release install install-man uninstall test clean

PREFIX ?= /usr/local
MANDIR ?= $(PREFIX)/share/man

all: doubletake doubletake-ctl doubletake-test-receiver

build: all

doubletake:
	go build -o bin/doubletake ./cmd/doubletake

doubletake-ctl:
	go build -o bin/doubletake-ctl ./cmd/doubletake-ctl

doubletake-test-receiver:
	go build -o bin/doubletake-test-receiver ./cmd/doubletake-test-receiver

doubletake-release:
	CGO_ENABLED=0 go build -ldflags='-s -w -extldflags=-static' -o doubletake ./cmd/doubletake

doubletake-ctl-release:
	CGO_ENABLED=0 go build -ldflags='-s -w -extldflags=-static' -o doubletake-ctl ./cmd/doubletake-ctl

doubletake-test-receiver-release:
	CGO_ENABLED=0 go build -ldflags='-s -w -extldflags=-static' -o doubletake-test-receiver ./cmd/doubletake-test-receiver

manpages-release:
	tar -czf doubletake-manpages.tar.gz -C man man1

test:
	go test ./...

install: all install-man
	install -m 755 bin/doubletake $(PREFIX)/bin/
	install -m 755 bin/doubletake-ctl $(PREFIX)/bin/
	install -m 755 bin/doubletake-test-receiver $(PREFIX)/bin/

install-man:
	install -d $(MANDIR)/man1
	install -m 644 man/man1/doubletake.1 $(MANDIR)/man1/
	install -m 644 man/man1/doubletake-ctl.1 $(MANDIR)/man1/
	install -m 644 man/man1/doubletake-test-receiver.1 $(MANDIR)/man1/

uninstall:
	rm -f $(PREFIX)/bin/doubletake
	rm -f $(PREFIX)/bin/doubletake-ctl
	rm -f $(PREFIX)/bin/doubletake-test-receiver
	rm -f $(MANDIR)/man1/doubletake.1
	rm -f $(MANDIR)/man1/doubletake-ctl.1
	rm -f $(MANDIR)/man1/doubletake-test-receiver.1

clean:
	rm -rf bin/
	go clean -testcache

.PHONY: helper helper-windows helper-linux helper-macos eld-encoder-windows eld-encoder-macos eld-encoder-linux
helper:
	go build -o bin/overflow-helper ./cmd/overflow-helper
	go build -o bin/airplay-feed ./cmd/airplay-feed

helper-windows:
	GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -o bin/windows/overflow-helper.exe ./cmd/overflow-helper
	GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -o bin/windows/airplay-feed.exe ./cmd/airplay-feed

helper-linux:
	GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -o bin/linux/overflow-helper ./cmd/overflow-helper
	GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -o bin/linux/airplay-feed ./cmd/airplay-feed

# Universal (arm64 + x86_64), signed so the plugin bundle's signature can seal
# it: ad hoc by default, or CODESIGN_IDENT (a Developer ID) with the hardened
# runtime and a timestamp, as notarization requires. macOS 12 is the oldest
# the Go toolchain supports; the plugin itself needs 13.
CODESIGN_IDENT ?= -
CODESIGN_FLAGS = $(if $(filter -,$(CODESIGN_IDENT)),,--options runtime --timestamp)
helper-macos:
	mkdir -p bin/macos/arm64 bin/macos/x86_64
	GOOS=darwin GOARCH=arm64 CGO_ENABLED=0 go build -o bin/macos/arm64/overflow-helper ./cmd/overflow-helper
	GOOS=darwin GOARCH=amd64 CGO_ENABLED=0 go build -o bin/macos/x86_64/overflow-helper ./cmd/overflow-helper
	lipo -create -output bin/macos/overflow-helper bin/macos/arm64/overflow-helper bin/macos/x86_64/overflow-helper
	codesign --force $(CODESIGN_FLAGS) --sign "$(CODESIGN_IDENT)" bin/macos/overflow-helper

# eld-encoder.exe (AAC-ELD, built from ../eld-encoder against fdk-aac) is a
# separate program with its own license. Building fdk-aac is slow, so
# helper-windows does not depend on this target. Output: bin/windows/eld-encoder.exe
eld-encoder-windows:
	../eld-encoder/build.sh windows

# The same encoder for macOS (universal) and Linux (x86_64), static fdk-aac.
eld-encoder-macos:
	../eld-encoder/build.sh macos
eld-encoder-linux:
	../eld-encoder/build.sh linux
