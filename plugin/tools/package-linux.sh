#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Christopher Gillespie
#
# Builds the Linux x86_64 package on Linux (Ubuntu 24.04 with the OBS PPA and
# the plugin-linux packages from .github/workflows/plugin.yml, plus Go):
#
#   plugin/release/obs-overflow-<version>-linux-x86_64.tar.gz
#
# The tarball holds one folder in OBS's per-user plugin layout, so it
# extracts straight into ~/.config/obs-studio/plugins (or the Flatpak's
# ~/.var/app/com.obsproject.Studio/config/obs-studio/plugins):
#
#   obs-overflow/bin/64bit/obs-overflow.so
#   obs-overflow/bin/64bit/overflow-helper        (plugin-main.cpp looks here)
#   obs-overflow/bin/64bit/eld-encoder            (the helper looks here)
#   obs-overflow/bin/64bit/eld-encoder-FDK-AAC-NOTICE.txt
#   obs-overflow/data/locale/en-US.ini
#   obs-overflow/data/LICENSE.txt, README.txt, licenses/
#
# SKIP_ELD=1 reuses an existing helper/bin/linux/eld-encoder (fdk-aac is slow).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PLUGIN="$ROOT/plugin"
[ "$(uname -s)" = Linux ] || { echo "run this on Linux" >&2; exit 1; }
[ "$(uname -m)" = x86_64 ] || { echo "x86_64 only for now (got $(uname -m))" >&2; exit 1; }

VERSION="$(sed -n 's/^ *"version": *"\([0-9.]*\)",*$/\1/p' "$PLUGIN/buildspec.json" | tail -n 1)"
[ -n "$VERSION" ] || { echo "no version in buildspec.json" >&2; exit 1; }

make -C "$ROOT/helper" helper-linux
if [ "${SKIP_ELD:-}" != 1 ] || [ ! -x "$ROOT/helper/bin/linux/eld-encoder" ]; then
	"$ROOT/eld-encoder/build.sh" linux
fi

cd "$PLUGIN"
cmake --preset ubuntu-x86_64 -DENABLE_CCACHE=OFF -DCMAKE_COMPILE_WARNING_AS_ERROR=ON \
	-DAIRPLAY_HELPER_BIN_DIR="$ROOT/helper/bin/linux"
cmake --build --preset ubuntu-x86_64 --parallel
INSTALL="$PLUGIN/release/linux-install"
rm -rf "$INSTALL"
cmake --install build_x86_64 --prefix "$INSTALL"

STAGE="$PLUGIN/release/linux-stage"
PKG="$STAGE/obs-overflow"
rm -rf "$STAGE"
mkdir -p "$PKG/bin/64bit" "$PKG/data"
LIB="$INSTALL/lib/x86_64-linux-gnu/obs-plugins"
for f in obs-overflow.so overflow-helper eld-encoder eld-encoder-FDK-AAC-NOTICE.txt; do
	[ -e "$LIB/$f" ] || { echo "missing from the install: $LIB/$f" >&2; exit 1; }
	cp -p "$LIB/$f" "$PKG/bin/64bit/"
done
cp -R "$INSTALL/share/obs/obs-plugins/obs-overflow/." "$PKG/data/"

expected=(
	bin/64bit/obs-overflow.so
	bin/64bit/overflow-helper
	bin/64bit/eld-encoder
	bin/64bit/eld-encoder-FDK-AAC-NOTICE.txt
	data/locale/en-US.ini
	data/LICENSE.txt
	data/README.txt
	data/licenses/nlohmann-json-MIT.txt
	data/licenses/doubletake-LGPL-3.0.txt
	data/licenses/eld-encoder-LICENSE-NOTICE.md
)
for f in "${expected[@]}"; do
	[ -e "$PKG/$f" ] || { echo "missing from the package: $f" >&2; exit 1; }
done
for f in overflow-helper eld-encoder; do
	[ -x "$PKG/bin/64bit/$f" ] || { echo "not executable: $f" >&2; exit 1; }
	file "$PKG/bin/64bit/$f" | grep -q "statically linked" || { echo "not static: $f" >&2; exit 1; }
done

OUT="$PLUGIN/release/obs-overflow-$VERSION-linux-x86_64.tar.gz"
tar -C "$STAGE" --owner=0 --group=0 -czf "$OUT" obs-overflow
echo "built $OUT"
tar -tzvf "$OUT"
sha256sum "$OUT"
