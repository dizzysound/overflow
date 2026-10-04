#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Christopher Gillespie
#
# Builds the macOS universal package on a Mac with Xcode (OBS 32.2.2 needs the
# macOS 26.5 SDK or later), Go and CMake:
#
#   plugin/release/obs-overflow-<version>-macos-universal.pkg
#     installs obs-overflow.plugin into /Library/Application Support/obs-studio/plugins
#   plugin/release/obs-overflow-<version>-macos-universal.zip
#     the bare obs-overflow.plugin, for ~/Library/Application Support/obs-studio/plugins
#
# Both are ad-hoc signed, not notarized. CODESIGN_IDENT and CODESIGN_TEAM
# (environment) select a Developer ID instead, for the bundle and the helper
# programs inside it; notarizing the result is a separate step.
#
# SKIP_ELD=1 reuses an existing helper/bin/macos/eld-encoder (fdk-aac is slow).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PLUGIN="$ROOT/plugin"
[ "$(uname -s)" = Darwin ] || { echo "run this on macOS" >&2; exit 1; }

VERSION="$(sed -n 's/^ *"version": *"\([0-9.]*\)",*$/\1/p' "$PLUGIN/buildspec.json" | tail -n 1)"
[ -n "$VERSION" ] || { echo "no version in buildspec.json" >&2; exit 1; }
export CODESIGN_IDENT="${CODESIGN_IDENT:--}"

make -C "$ROOT/helper" helper-macos
if [ "${SKIP_ELD:-}" != 1 ] || [ ! -x "$ROOT/helper/bin/macos/eld-encoder" ]; then
	"$ROOT/eld-encoder/build.sh" macos
fi

cd "$PLUGIN"
cmake --preset macos -DENABLE_CCACHE=OFF -DCMAKE_COMPILE_WARNING_AS_ERROR=ON \
	-DAIRPLAY_HELPER_BIN_DIR="$ROOT/helper/bin/macos"
cmake --build --preset macos
INSTALL="$PLUGIN/release/macos-install"
rm -rf "$INSTALL"
# The install script also runs pkgbuild/productbuild (cmake/macos/resources).
cmake --install build_macos --config RelWithDebInfo --prefix "$INSTALL"

BUNDLE="$INSTALL/obs-overflow.plugin"
expected=(
	Contents/MacOS/obs-overflow
	Contents/MacOS/overflow-helper
	Contents/MacOS/eld-encoder
	Contents/Resources/locale/en-US.ini
	Contents/Resources/LICENSE.txt
	Contents/Resources/README.txt
	Contents/Resources/licenses/nlohmann-json-MIT.txt
	Contents/Resources/licenses/doubletake-LGPL-3.0.txt
	Contents/Resources/licenses/eld-encoder-LICENSE-NOTICE.md
	Contents/Resources/licenses/eld-encoder-FDK-AAC-NOTICE.txt
)
for f in "${expected[@]}"; do
	[ -e "$BUNDLE/$f" ] || { echo "missing from the bundle: $f" >&2; exit 1; }
done
for f in obs-overflow overflow-helper eld-encoder; do
	archs="$(lipo -archs "$BUNDLE/Contents/MacOS/$f")"
	case "$archs" in
	"x86_64 arm64" | "arm64 x86_64") ;;
	*) echo "not universal: $f ($archs)" >&2; exit 1 ;;
	esac
done
codesign --verify --deep --strict "$BUNDLE"
[ -f "$INSTALL/obs-overflow.pkg" ] || { echo "the install step made no .pkg" >&2; exit 1; }

PKG="$PLUGIN/release/obs-overflow-$VERSION-macos-universal.pkg"
ZIP="$PLUGIN/release/obs-overflow-$VERSION-macos-universal.zip"
cp "$INSTALL/obs-overflow.pkg" "$PKG"
rm -f "$ZIP"
ditto -c -k --keepParent "$BUNDLE" "$ZIP"
pkgutil --payload-files "$PKG" | grep -E "obs-overflow.plugin/Contents/MacOS/" || {
	echo "the .pkg payload has no plugin binaries" >&2
	exit 1
}
echo "built $PKG"
echo "built $ZIP"
shasum -a 256 "$PKG" "$ZIP"
