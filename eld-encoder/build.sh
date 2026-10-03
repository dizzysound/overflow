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
*)
	echo "usage: $0 native|windows" >&2
	exit 2
	;;
esac
