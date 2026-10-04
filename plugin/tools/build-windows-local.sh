#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Christopher Gillespie
# Cross-build obs-overflow.dll (Windows x64, MSVC ABI) on macOS with clang-cl + lld-link,
# then assemble obs-overflow-<ver>-windows-x64.zip in the CI layout.
#
# Inputs (read-only): the repo's plugin/ and helper/ sources ($REPO, default: this
# checkout). Toolchain, deps, build tree and output live under $WINBUILD_DIR
# (default ~/.cache/obs-airplay-winbuild; about 2.5 GB). eld-encoder.exe and its
# notice are taken from a previous package zip ($ELD_ZIP), since it rarely changes.
# Used while GitHub Actions is unavailable; CI remains the reference build.
#
# Prereqs: brew install llvm lld xwin qtbase go cmake
#   xwin requires accepting the Microsoft Visual Studio license (--accept-license).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
W="${WINBUILD_DIR:-$HOME/.cache/obs-airplay-winbuild}"; mkdir -p "$W"
REPO="${REPO:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
P="$REPO/plugin"
# The cache folder and the eld-encoder source zip keep their pre-rename (obs-airplay) names.
CI_ZIP="${ELD_ZIP:-${CI_ZIP:-$W/eld/obs-airplay-windows-x64.zip}}"
LLVM=/opt/homebrew/opt/llvm/bin
LLD=/opt/homebrew/opt/lld/bin
MOC=/opt/homebrew/opt/qtbase/share/qt/libexec/moc
JOBS="$(sysctl -n hw.ncpu)"

NAME=obs-overflow
VERSION="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["version"])' "$P/buildspec.json")"
DEPS="$W/deps"; XWIN="$W/xwin"; B="$W/build"; OUT="$W/out"
OBS_VER=32.2.2; DEPS_VER=2026-07-15
OBS_SRC="$DEPS/obs-studio-$OBS_VER"; QT="$DEPS/qt6"

mkdir -p "$DEPS" "$B/obj" "$B/gen" "$OUT"

# ---------------------------------------------------------------- 1. sysroot
if [ ! -d "$XWIN/crt/include" ]; then
  xwin --accept-license --arch x86_64 --cache-dir "$W/xwin-cache" splat --output "$XWIN"
fi

# ---------------------------------------------------------------- 2. deps (hashes from buildspec.json)
fetch() { # url file sha256
  [ -f "$DEPS/$2" ] || curl -sSLf -o "$DEPS/$2" "$1"
  echo "$3  $DEPS/$2" | shasum -a 256 -c -
}
fetch "https://github.com/obsproject/obs-studio/archive/refs/tags/$OBS_VER.tar.gz" "obs-studio-$OBS_VER.tar.gz" \
  35d3cd0979d65664fada7119fdb612eca7c34b61a1623a330caec74bf72626c4
fetch "https://github.com/obsproject/obs-deps/releases/download/$DEPS_VER/windows-deps-qt6-$DEPS_VER-x64.zip" "windows-deps-qt6-$DEPS_VER-x64.zip" \
  7c7f985711d80467bdc1795b6592275a27d5b0e5a2c7a61db1f2c1d08d6a5579
[ -d "$OBS_SRC" ] || tar xzf "$DEPS/obs-studio-$OBS_VER.tar.gz" -C "$DEPS"
[ -d "$QT/include" ] || { mkdir -p "$QT"; unzip -q -o "$DEPS/windows-deps-qt6-$DEPS_VER-x64.zip" -d "$QT"; }
# PATCH (deps copy of OBS headers, not the repo): clang-cl defines _MSC_VER but has no
# _udiv128 intrinsic, so take util_mul_div64's portable branch (the one Linux/macOS use).
sed -i '' 's/#if defined(_MSC_VER) \&\& defined(_M_X64) \&\& (_MSC_VER >= 1920)$/#if defined(_MSC_VER) \&\& defined(_M_X64) \&\& (_MSC_VER >= 1920) \&\& !defined(__clang__)/' \
  "$OBS_SRC/libobs/util/util_uint64.h"
grep -q '!defined(__clang__)' "$OBS_SRC/libobs/util/util_uint64.h"
# (obs-deps windows-deps-*.zip is not needed: the plugin links none of its libraries.)

# Host moc must share the Windows Qt headers' Q_MOC_OUTPUT_REVISION.
WIN_REV=$(grep -h "define Q_MOC_OUTPUT_REVISION" "$QT/include/QtCore/"*.h | awk '{print $3}' | tr -d '\r ')
HOST_REV=$(grep -h "define Q_MOC_OUTPUT_REVISION" /opt/homebrew/opt/qtbase/lib/QtCore.framework/Headers/*.h | awk '{print $3}' | tr -d '\r ')
echo "moc revision: windows headers=$WIN_REV host=$HOST_REV ($($MOC -v))"
[ "$WIN_REV" = "$HOST_REV" ] || { echo "moc revision mismatch" >&2; exit 1; }

# ---------------------------------------------------------------- 3. generated sources
GEN="$B/gen"
mkdir -p "$GEN/include"
sed -e 's/#cmakedefine .*//' -e 's/@OBS_RELEASE_CANDIDATE@/0/' -e 's/@OBS_BETA@/0/' \
  "$OBS_SRC/libobs/obsconfig.h.in" > "$GEN/include/obsconfig.h"
sed -e "s/@CMAKE_PROJECT_NAME@/$NAME/" -e "s/@CMAKE_PROJECT_VERSION@/$VERSION/" \
  "$P/src/plugin-support.c.in" > "$GEN/plugin-support.c"
IFS=. read -r VMAJ VMIN VPAT <<< "$VERSION"
AUTHOR="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["author"])' "$P/buildspec.json")"
sed -e "s/\${PROJECT_VERSION_MAJOR}/$VMAJ/g" -e "s/\${PROJECT_VERSION_MINOR}/$VMIN/g" \
    -e "s/\${PROJECT_VERSION_PATCH}/$VPAT/g" -e "s/\${PROJECT_VERSION}/$VERSION/g" \
    -e "s/\${PROJECT_NAME}/$NAME/g" -e "s/\${PLUGIN_AUTHOR}/$AUTHOR/g" -e "s/\${CURRENT_YEAR}/$(date +%Y)/g" \
  "$P/cmake/windows/resources/resource.rc.in" > "$GEN/$NAME.rc"

# moc every header that declares Q_OBJECT (what CMake AUTOMOC would do).
MOC_SRCS=()
for h in $(grep -l -E 'Q_OBJECT|Q_GADGET' -r "$P/src" --include='*.hpp' --include='*.h'); do
  base=$(basename "$h" .hpp)
  "$MOC" -DWIN32 -D_WIN32 -D_WIN64 -D_MSC_VER=1944 -DQ_OS_WIN -I"$QT/include" -f"$(realpath --relative-to="$P/src" "$h" 2>/dev/null || basename "$h")" \
    "$h" -o "$GEN/moc_$base.cpp"
  MOC_SRCS+=("$GEN/moc_$base.cpp")
done

# ---------------------------------------------------------------- 4. compile
TARGET=--target=x86_64-pc-windows-msvc
SYS=(/imsvc "$XWIN/crt/include" /imsvc "$XWIN/sdk/include/ucrt" /imsvc "$XWIN/sdk/include/um"
     /imsvc "$XWIN/sdk/include/shared" /imsvc "$XWIN/sdk/include/winrt")
# RelWithDebInfo as in CI (/O2 /Ob1 /DNDEBUG /Zi /MD); plugin template's /W3 /utf-8 /Brepro /permissive-.
COMMON=("$TARGET" /nologo /c /MD /O2 /Ob1 /DNDEBUG /Z7 /Gy /Oi /W3 /utf-8 /Brepro /permissive- /Zc:__cplusplus
        -fms-compatibility-version=19.44 -fno-strict-aliasing
        -DUNICODE -D_UNICODE -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_WARNINGS -DWIN32 -D_WINDOWS
        "${SYS[@]}")
# Clang warning set that the template applies to clang-cl (compiler_common.cmake), minus ObjC-only flags.
CLANG_W=(-Wno-trigraphs -Wno-missing-field-initializers -Wno-missing-prototypes -Werror=return-type
         -Wunreachable-code -Wno-missing-braces -Wparentheses -Wswitch -Wno-unused-function -Wno-unused-label
         -Wunused-parameter -Wunused-variable -Wunused-value -Wempty-body -Wuninitialized -Wno-unknown-pragmas
         -Wfour-char-constants -Wconstant-conversion -Wno-conversion -Wint-conversion -Wbool-conversion
         -Wenum-conversion -Wnon-literal-null-conversion -Wsign-compare -Wshorten-64-to-32 -Wpointer-sign
         -Wnewline-eof -Wno-implicit-fallthrough -Wdeprecated-declarations -Wno-sign-conversion
         -Winfinite-recursion -Wno-strict-prototypes -Wno-semicolon-before-method-body -Wformat-security -Wvla
         -Wno-error=shorten-64-to-32 -Wno-shadow)
CXXFLAGS=(/std:c++17 /EHsc "${CLANG_W[@]}" -Wno-non-virtual-dtor -Wno-overloaded-virtual -Wno-exit-time-destructors
          -Winvalid-offsetof -Wmove -Wrange-loop-analysis)
CFLAGS=(/std:c17 "${CLANG_W[@]}" -Wno-float-conversion)

# airplay-core (core/CMakeLists.txt adds /W3 /WX /utf-8 under MSVC; mirrored with /WX).
CORE_INC=(-I"$P/core/include" /imsvc "$P/core/third_party")
QT_DEFS=(-DQT_CORE_LIB -DQT_GUI_LIB -DQT_WIDGETS_LIB -DQT_NETWORK_LIB -DQT_NO_DEBUG
         -D_ENABLE_EXTENDED_ALIGNED_STORAGE -DWIN64 -D_WIN64)
QT_INC=(/imsvc "$QT/include" /imsvc "$QT/include/QtCore" /imsvc "$QT/include/QtGui"
        /imsvc "$QT/include/QtWidgets" /imsvc "$QT/include/QtNetwork" /imsvc "$QT/mkspecs/win32-msvc")
OBS_INC=(/imsvc "$OBS_SRC/libobs" /imsvc "$OBS_SRC/frontend/api" /imsvc "$GEN/include")
PLUGIN_INC=(-I"$P/src" -I"$P/src/ui" "${CORE_INC[@]}" "${OBS_INC[@]}" "${QT_INC[@]}")

WARN_LOG="$B/warnings.log"; : > "$WARN_LOG"
cc_one() { # lang src obj flags...
  local lang=$1 src=$2 obj=$3; shift 3
  "$LLVM/clang-cl" "${COMMON[@]}" "$@" /Fo"$obj" -- "$src" 2>>"$WARN_LOG"
}
export -f cc_one; export LLVM WARN_LOG

jobs_file="$B/jobs.txt"; : > "$jobs_file"
CORE_OBJS=(); PLUGIN_OBJS=()
for s in "$P"/core/src/*.cpp; do
  case "$(basename "$s")" in process_posix.cpp | secret_macos.cpp | secret_linux.cpp) continue ;; esac # not Windows sources
  o="$B/obj/core_$(basename "$s" .cpp).obj"; CORE_OBJS+=("$o")
  printf '%s\0' "$s" "$o" "core" >> "$jobs_file"
done
for s in "$P"/src/*.cpp "$P"/src/ui/*.cpp "${MOC_SRCS[@]}"; do
  o="$B/obj/plugin_$(basename "$s" .cpp).obj"; PLUGIN_OBJS+=("$o")
  printf '%s\0' "$s" "$o" "plugin" >> "$jobs_file"
done
printf '%s\0' "$GEN/plugin-support.c" "$B/obj/plugin-support.obj" "support" >> "$jobs_file"

compile() {
  local s=$1 o=$2 kind=$3
  case $kind in
    core)    cc_one cxx "$s" "$o" "${CXXFLAGS[@]}" /WX "${CORE_INC[@]}" ;;
    plugin)  cc_one cxx "$s" "$o" "${CXXFLAGS[@]}" "${QT_DEFS[@]}" "${PLUGIN_INC[@]}" ;;
    support) cc_one c   "$s" "$o" "${CFLAGS[@]}" -I"$P/src" "${OBS_INC[@]}" ;;
  esac && echo "  cc $(basename "$s")"
}
# Serialize arrays for xargs subshells.
export COMMON_S="$(declare -p COMMON)" CXX_S="$(declare -p CXXFLAGS)" C_S="$(declare -p CFLAGS)" \
  CORE_S="$(declare -p CORE_INC)" QTD_S="$(declare -p QT_DEFS)" PINC_S="$(declare -p PLUGIN_INC)" \
  OINC_S="$(declare -p OBS_INC)" P
export -f compile
xargs -0 -n3 -P "$JOBS" bash -c 'eval "$COMMON_S"; eval "$CXX_S"; eval "$C_S"; eval "$CORE_S"; eval "$QTD_S"; eval "$PINC_S"; eval "$OINC_S"; compile "$@"' _ < "$jobs_file"
echo "compiler diagnostics: $(grep -c 'warning:' "$WARN_LOG" || true) warnings, $(grep -c 'error:' "$WARN_LOG" || true) errors (see $WARN_LOG)"

"$LLVM/llvm-lib" /nologo /out:"$B/airplay-core.lib" "${CORE_OBJS[@]}"

# ---------------------------------------------------------------- 5. import libs for obs.dll / obs-frontend-api.dll
# Derived from source: every unmangled C symbol (bare or __imp_) the objects need that is declared EXPORT in
# libobs headers (-> obs.dll) or in obs-frontend-api.h (-> obs-frontend-api.dll).
ALL_OBJS=("${PLUGIN_OBJS[@]}" "$B/obj/plugin-support.obj" "${CORE_OBJS[@]}")
"$LLVM/llvm-nm" -u "${ALL_OBJS[@]}" 2>/dev/null | awk '{print $NF}' | sed 's/^__imp_//' \
  | grep -v '^?' | sort -u > "$B/imp_c_symbols.txt"
: > "$B/obs.syms"; : > "$B/obs-frontend-api.syms"
while read -r sym; do
  if grep -q -E "EXPORT[^;(]*[ *]$sym *\(" "$OBS_SRC/frontend/api/obs-frontend-api.h"; then
    echo "$sym" >> "$B/obs-frontend-api.syms"
  elif grep -r -q -E "EXPORT[^;(]*[ *]$sym *\(" "$OBS_SRC/libobs" --include='*.h'; then
    echo "$sym" >> "$B/obs.syms"
  fi
done < "$B/imp_c_symbols.txt"
for d in obs obs-frontend-api; do
  { echo "LIBRARY $d.dll"; echo "EXPORTS"; sed 's/^/    /' "$B/$d.syms"; } > "$B/$d.def"
  "$LLVM/llvm-dlltool" -m i386:x86-64 -d "$B/$d.def" -l "$B/$d.lib"
done
echo "import libs: obs.dll $(wc -l < "$B/obs.syms") syms, obs-frontend-api.dll $(wc -l < "$B/obs-frontend-api.syms") syms"

# ---------------------------------------------------------------- 6. resources + link
( cd "$B" && "$LLVM/llvm-rc" /nologo /fo "$NAME.res" "gen/$NAME.rc" )
"$LLD/lld-link" /nologo /dll /machine:x64 /out:"$B/$NAME.dll" /pdb:"$B/$NAME.pdb" /debug \
  /opt:ref /opt:icf /incremental:no /Brepro \
  /libpath:"$XWIN/crt/lib/x86_64" /libpath:"$XWIN/sdk/lib/um/x86_64" /libpath:"$XWIN/sdk/lib/ucrt/x86_64" \
  /libpath:"$QT/lib" \
  "${PLUGIN_OBJS[@]}" "$B/obj/plugin-support.obj" "$B/airplay-core.lib" "$B/$NAME.res" \
  "$B/obs.lib" "$B/obs-frontend-api.lib" Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib Qt6Network.lib \
  crypt32.lib kernel32.lib user32.lib advapi32.lib shell32.lib ole32.lib ws2_32.lib \
  msvcrt.lib vcruntime.lib ucrt.lib msvcprt.lib
echo "linked $B/$NAME.dll"

# ---------------------------------------------------------------- 7. helper + package
HB="$B/helper-bin"; mkdir -p "$HB"
( cd "$REPO/helper" && GOOS=windows GOARCH=amd64 CGO_ENABLED=0 go build -o "$HB/overflow-helper.exe" ./cmd/overflow-helper )
echo "${BUILD_COMMIT:-$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)}" > "$B/helper-commit.txt"
git -C "$REPO" log -1 --format='%H %s' -- helper > "$B/helper-last-change.txt" 2>/dev/null || true
git -C "$REPO" status --porcelain -- helper plugin > "$B/worktree-dirty.txt" 2>/dev/null || true
CI_X="$B/ci-artifact"; rm -rf "$CI_X"; mkdir -p "$CI_X"; unzip -q -o "$CI_ZIP" -d "$CI_X"
# A CI zip from before or after the rename: obs-airplay/ or obs-overflow/.
ELD_DIR="$(dirname "$(find "$CI_X" -path '*/bin/64bit/eld-encoder.exe' | head -1)")"
cp "$ELD_DIR/eld-encoder.exe" "$ELD_DIR/eld-encoder-FDK-AAC-NOTICE.txt" "$HB/"

STAGE="$B/stage"; rm -rf "$STAGE"; R="$STAGE/$NAME"
mkdir -p "$R/bin/64bit" "$R/data" "$R/licenses"
cp "$B/$NAME.dll" "$B/$NAME.pdb" "$HB/overflow-helper.exe" "$HB/eld-encoder.exe" "$HB/eld-encoder-FDK-AAC-NOTICE.txt" "$R/bin/64bit/"
cp -R "$P/data/" "$R/data/"
cp "$P/LICENSE" "$R/LICENSE.txt"; cp "$P/README.md" "$R/README.txt"
cp "$P/core/third_party/nlohmann/LICENSE.MIT" "$R/licenses/nlohmann-json-MIT.txt"
cp "$REPO/helper/LICENSE" "$R/licenses/doubletake-LGPL-3.0.txt"
cp "$REPO/eld-encoder/LICENSE-NOTICE.md" "$R/licenses/eld-encoder-LICENSE-NOTICE.md"
for f in bin/64bit/$NAME.dll bin/64bit/overflow-helper.exe bin/64bit/eld-encoder.exe \
         bin/64bit/eld-encoder-FDK-AAC-NOTICE.txt data/locale/en-US.ini LICENSE.txt README.txt \
         licenses/nlohmann-json-MIT.txt licenses/doubletake-LGPL-3.0.txt licenses/eld-encoder-LICENSE-NOTICE.md; do
  [ -f "$R/$f" ] || { echo "missing from the package: $f" >&2; exit 1; }
done
ZIP="$OUT/$NAME-$VERSION-windows-x64.zip"; rm -f "$ZIP"
( cd "$STAGE" && zip -q -r -X "$ZIP" "$NAME" )
echo "package: $ZIP"; unzip -l "$ZIP"
