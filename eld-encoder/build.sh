#!/usr/bin/env bash
# Build eld-encoder.
#
#   ./eld-encoder/build.sh native    macOS/Linux build against the system
#                                    fdk-aac (pkg-config), for tests.
#                                    Output: eld-encoder/build/native/eld-encoder
#   ./eld-encoder/build.sh windows   Windows x86-64 cross-build. Downloads the
#                                    sha256-pinned fdk-aac source, builds it as
#                                    a static library with mingw-w64, and links
#                                    a fully static eld-encoder.exe.
#                                    Output: helper/bin/windows/eld-encoder.exe
#   ./eld-encoder/build.sh macos     macOS universal (arm64 + x86_64) build
#                                    against the same pinned fdk-aac, static,
#                                    signed (ad hoc, or $CODESIGN_IDENT),
#                                    macOS 13 or later.
#                                    Output: helper/bin/macos/eld-encoder
#   ./eld-encoder/build.sh linux     Linux build (run it on Linux) against the
#                                    same pinned fdk-aac, fully static.
#                                    Output: helper/bin/linux/eld-encoder
#
# Build intermediates live in eld-encoder/build/ (git-ignored). fdk-aac source
# is never committed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$HERE/build"

FDK_VERSION=2.0.3
FDK_URL="https://github.com/mstorsjo/fdk-aac/archive/refs/tags/v${FDK_VERSION}.tar.gz"
FDK_SHA256=e25671cd96b10bad896aa42ab91a695a9e573395262baed4e4a2ff178d6a3a78

MINGW_PREFIX="${MINGW_PREFIX:-x86_64-w64-mingw32}"

sha256_of() {
	if command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | awk '{print $1}'
	else
		sha256sum "$1" | awk '{print $1}'
	fi
}

build_native() {
	local out="$BUILD/native"
	mkdir -p "$out"
	# shellcheck disable=SC2046
	cc -O2 -Wall -Wextra -Wno-unused-function -o "$out/eld-encoder" "$HERE/main.c" \
		$(pkg-config --cflags --libs fdk-aac)
	echo "built $out/eld-encoder"
}

fetch_fdk() {
	local tarball="$BUILD/fdk-aac-${FDK_VERSION}.tar.gz"
	mkdir -p "$BUILD"
	if [ ! -f "$tarball" ] || [ "$(sha256_of "$tarball")" != "$FDK_SHA256" ]; then
		rm -f "$tarball"
		curl -fsSL -o "$tarball.part" "$FDK_URL"
		mv "$tarball.part" "$tarball"
	fi
	local got
	got="$(sha256_of "$tarball")"
	if [ "$got" != "$FDK_SHA256" ]; then
		echo "fdk-aac tarball sha256 mismatch: got $got, want $FDK_SHA256" >&2
		rm -f "$tarball"
		exit 1
	fi
	rm -rf "$BUILD/fdk-aac-${FDK_VERSION}"
	tar -xzf "$tarball" -C "$BUILD"
}

build_windows() {
	local cc="${MINGW_PREFIX}-gcc"
	command -v "$cc" >/dev/null || { echo "missing $cc (install mingw-w64)" >&2; exit 1; }
	fetch_fdk

	local toolchain="$BUILD/mingw-toolchain.cmake"
	cat >"$toolchain" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER ${MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${MINGW_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

	local fdk_build="$BUILD/fdk-aac-windows"
	local prefix="$BUILD/windows-prefix"
	rm -rf "$fdk_build" "$prefix"
	cmake -S "$BUILD/fdk-aac-${FDK_VERSION}" -B "$fdk_build" \
		-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$prefix" \
		-DBUILD_SHARED_LIBS=OFF \
		-DBUILD_PROGRAMS=OFF \
		-DFDK_AAC_INSTALL_CMAKE_CONFIG_MODULE=OFF \
		-DFDK_AAC_INSTALL_PKGCONFIG_MODULE=OFF
	cmake --build "$fdk_build" --parallel
	cmake --install "$fdk_build"

	local exe="$ROOT/helper/bin/windows/eld-encoder.exe"
	mkdir -p "$(dirname "$exe")"
	"$cc" -O2 -Wall -Wextra -Wno-unused-function -static -o "$exe" "$HERE/main.c" \
		-I"$prefix/include" -L"$prefix/lib" -lfdk-aac -lm
	"${MINGW_PREFIX}-strip" "$exe"
	# The FDK license must travel with the binary.
	cp "$BUILD/fdk-aac-${FDK_VERSION}/NOTICE" "$(dirname "$exe")/eld-encoder-FDK-AAC-NOTICE.txt"

	echo "built $exe"
	file "$exe"
	echo "DLL dependencies:"
	"${MINGW_PREFIX}-objdump" -p "$exe" | grep "DLL Name" || true
	verify_windows_imports "$exe"
}

# Builds the pinned fdk-aac as a static library into $2 with extra CMake args.
build_fdk_static() {
	local fdk_build="$1" prefix="$2"
	shift 2
	rm -rf "$fdk_build" "$prefix"
	cmake -S "$BUILD/fdk-aac-${FDK_VERSION}" -B "$fdk_build" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$prefix" \
		-DCMAKE_INSTALL_LIBDIR=lib \
		-DBUILD_SHARED_LIBS=OFF \
		-DBUILD_PROGRAMS=OFF \
		-DFDK_AAC_INSTALL_CMAKE_CONFIG_MODULE=OFF \
		-DFDK_AAC_INSTALL_PKGCONFIG_MODULE=OFF \
		"$@"
	cmake --build "$fdk_build" --parallel
	cmake --install "$fdk_build"
}

build_macos() {
	[ "$(uname -s)" = Darwin ] || { echo "build.sh macos runs on macOS" >&2; exit 1; }
	fetch_fdk
	local prefix="$BUILD/macos-prefix"
	build_fdk_static "$BUILD/fdk-aac-macos" "$prefix" \
		-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0

	local exe="$ROOT/helper/bin/macos/eld-encoder"
	mkdir -p "$(dirname "$exe")"
	cc -O2 -Wall -Wextra -Wno-unused-function -arch arm64 -arch x86_64 -mmacosx-version-min=13.0 \
		-o "$exe" "$HERE/main.c" -I"$prefix/include" "$prefix/lib/libfdk-aac.a" -lc++
	strip -x "$exe"
	# Ad hoc by default; a Developer ID (CODESIGN_IDENT) also gets the hardened
	# runtime and a timestamp, as notarization requires.
	local ident="${CODESIGN_IDENT:--}"
	if [ "$ident" = "-" ]; then
		codesign --force --sign - "$exe"
	else
		codesign --force --options runtime --timestamp --sign "$ident" "$exe"
	fi
	cp "$BUILD/fdk-aac-${FDK_VERSION}/NOTICE" "$(dirname "$exe")/eld-encoder-FDK-AAC-NOTICE.txt"

	echo "built $exe"
	file "$exe"
	# Only system libraries: no Homebrew or fdk-aac dylib.
	# Dependency lines are indented; the others name the file and architecture.
	if otool -L "$exe" | awk '/^[[:space:]]/ {print $1}' | grep -v -E '^/usr/lib/|^/System/'; then
		echo "unexpected dylib dependency" >&2
		exit 1
	fi
	echo "dylibs OK: system only"
}

build_linux() {
	[ "$(uname -s)" = Linux ] || { echo "build.sh linux runs on Linux (or in a container)" >&2; exit 1; }
	fetch_fdk
	local prefix="$BUILD/linux-prefix"
	build_fdk_static "$BUILD/fdk-aac-linux" "$prefix"

	local exe="$ROOT/helper/bin/linux/eld-encoder"
	mkdir -p "$(dirname "$exe")"
	cc -O2 -Wall -Wextra -Wno-unused-function -static -o "$exe" "$HERE/main.c" \
		-I"$prefix/include" -L"$prefix/lib" -lfdk-aac -lm
	strip "$exe"
	cp "$BUILD/fdk-aac-${FDK_VERSION}/NOTICE" "$(dirname "$exe")/eld-encoder-FDK-AAC-NOTICE.txt"

	echo "built $exe"
	file "$exe"
	file "$exe" | grep -q "statically linked" || { echo "not statically linked: $exe" >&2; exit 1; }
}

# The .exe must load with nothing but Windows system DLLs next to it.
verify_windows_imports() {
	local exe="$1" dll bad=0
	file "$exe" | grep -q "PE32+ executable.*x86-64" || { echo "not a PE32+ x86-64 executable: $exe" >&2; exit 1; }
	file "$exe" | grep -q "(console)" || { echo "not a console executable: $exe" >&2; exit 1; }
	while read -r dll; do
		case "$(printf '%s' "$dll" | tr '[:upper:]' '[:lower:]')" in
		kernel32.dll | msvcrt.dll | ucrtbase.dll | api-ms-win-*.dll | advapi32.dll | user32.dll | ntdll.dll | ws2_32.dll | bcrypt.dll) ;;
		*)
			echo "unexpected DLL dependency: $dll" >&2
			bad=1
			;;
		esac
	done < <("${MINGW_PREFIX}-objdump" -p "$exe" | awk '/DLL Name:/ {print $3}')
	[ "$bad" -eq 0 ] || exit 1
	echo "imports OK: system DLLs only"
}

case "${1:-}" in
native) build_native ;;
windows) build_windows ;;
macos) build_macos ;;
linux) build_linux ;;
*)
	echo "usage: $0 native|windows|macos|linux" >&2
	exit 2
	;;
esac
