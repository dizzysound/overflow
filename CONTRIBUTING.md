# Contributing to Overflow

Thanks for helping. Bug reports, receiver compatibility reports and pull requests are all welcome.

## Reporting a problem

Open an issue with the **Bug report** form. The most useful things to attach are:

- The diagnostic report: **Tools > Overflow: Save Diagnostic Log...** (secrets are redacted).
- The OBS log (**Help > Log Files**), cut to the lines starting with `[obs-overflow]`.
- The receiver's make and model, and how it is connected (Ethernet or Wi-Fi).

If a receiver works, half works or doesn't work at all, the **Receiver report** form helps other
people find out before they buy hardware.

## Building and testing

The plugin logic in `plugin/core/` builds and tests without OBS or Qt:

```bash
cmake -S plugin/tests -B plugin/build-tests
cmake --build plugin/build-tests
ctest --test-dir plugin/build-tests --output-on-failure
```

The helper is a Go module:

```bash
cd helper
go vet ./...
go test ./...
make helper            # bin/overflow-helper and bin/airplay-feed
make helper-windows    # Windows cross-build
```

The full plugin builds against OBS 32.2.2 in GitHub Actions (`.github/workflows/plugin.yml`). On a
Mac, `plugin/tools/build-windows-local.sh` cross-builds the Windows package with clang-cl.

`helper/cmd/airplay-feed` drives the helper with a built-in test clip and tone, which is the
quickest way to test against a receiver without OBS.

## Pull requests

- Keep each pull request to one change, with tests for logic in `plugin/core/` or `helper/`.
- Run the core tests and `go test ./...` before opening it, and say in the description what you
  tested on real receivers, if anything.
- `helper/` is a subtree of [doubletake](https://github.com/omarroth/doubletake). Fixes to the
  AirPlay sender itself that aren't specific to Overflow are worth offering upstream too.
- Formatting: `.clang-format` for C++, `gofmt` for Go.

## Code layout

| Path | What |
|---|---|
| `plugin/src/` | OBS-facing code: module entry, output, controller, Qt dock and dialogs |
| `plugin/core/` | Logic without OBS or Qt: settings, protocol, supervisor, latency policy |
| `helper/cmd/overflow-helper/` | The helper's entry point |
| `helper/internal/airplay/` | AirPlay sessions, pairing, RTP, timing |
| `helper/internal/bridge/` | The plugin protocol inside the helper |
| `docs/protocol.md` | The plugin-to-helper protocol |

## License

By contributing you agree that your contribution is licensed under GPLv3, or LGPL-3.0 for code
under `helper/`.
