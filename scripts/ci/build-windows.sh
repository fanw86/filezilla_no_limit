#!/usr/bin/env bash
# Build FileZilla for Windows (MSYS2) and produce the NSIS setup installer.
#
# Environment:
#   PREFIX      - dependency install root (libfilezilla, fzssh)  [required]
#   SOURCE_DIR  - FileZilla source tree (default: filezilla3)
#   OUT_DIR     - artifact output directory (default: dist)
#   WITH_SHELLEXT=1 - also build 32/64-bit shell extension (needs i686 toolchain)
set -euo pipefail

PREFIX="${PREFIX:-$HOME/prefix}"
SOURCE_DIR="${SOURCE_DIR:-filezilla3}"
OUT_DIR="${OUT_DIR:-dist}"
WITH_SHELLEXT="${WITH_SHELLEXT:-1}"
WORKERS="${WORKERS:-$(nproc 2>/dev/null || echo 2)}"

export PATH="$PREFIX/bin:/mingw64/bin:/mingw32/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:/mingw64/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib64:/mingw64/bin:${LD_LIBRARY_PATH:-}"
export LC_ALL=C

# MSYS2 windres is missing the target prefix some autotools checks expect.
if [[ ! -e /mingw64/bin/x86_64-w64-mingw32-windres.exe && -e /mingw64/bin/windres.exe ]]; then
	ln -sf /mingw64/bin/windres.exe /mingw64/bin/x86_64-w64-mingw32-windres.exe || true
fi
if [[ ! -e /mingw32/bin/i686-w64-mingw32-windres.exe && -e /mingw32/bin/windres.exe ]]; then
	ln -sf /mingw32/bin/windres.exe /mingw32/bin/i686-w64-mingw32-windres.exe || true
fi

log() { echo "==> $*"; }

SRC="$(cd "$SOURCE_DIR" && pwd)"
BUILD="$SRC/compile"
mkdir -p "$OUT_DIR"
OUT="$(cd "$OUT_DIR" && pwd)"

shellext_flags=(--disable-shellext)
if [[ "$WITH_SHELLEXT" == "1" ]]; then
	shellext_flags=(--enable-shellext)
	log "Shell extension enabled (needs i686 + x86_64 mingw toolchains)"
else
	log "Shell extension disabled"
fi

log "Running autoreconf in $SRC"
# automake requires these SUBDIRS/DIST_SUBDIRS paths to exist
mkdir -p "$SRC/src/fzshellext/32" "$SRC/src/fzshellext/64"
pushd "$SRC" >/dev/null
autoreconf -fi
popd >/dev/null

log "Configuring FileZilla"
rm -rf "$BUILD"
mkdir -p "$BUILD"
pushd "$BUILD" >/dev/null
"$SRC/configure" \
	--prefix="$PREFIX" \
	--enable-shared \
	--disable-static \
	--with-pugixml=builtin \
	"${shellext_flags[@]}"

log "Compiling FileZilla (-j$WORKERS)"
make -j"$WORKERS"

log "Stripping binaries"
strip_bin() {
	local f="$1"
	if [[ -f "$f" ]]; then
		strip "$f" || true
		echo "    stripped $f"
	fi
}
strip_bin "src/interface/.libs/filezilla.exe"
strip_bin "src/interface/filezilla.exe"
strip_bin "src/storj/.libs/fzstorj.exe"
strip_bin "src/storj/fzstorj.exe"
strip_bin "src/fzshellext/64/.libs/libfzshellext-0.dll"
strip_bin "src/fzshellext/32/.libs/libfzshellext-0.dll"
if [[ -d data/dlls ]]; then
	while IFS= read -r -d '' dll; do
		strip_bin "$dll"
	done < <(find data/dlls -name '*.dll' -print0)
fi

log "Building NSIS installer"
if [[ ! -f data/install.nsi ]]; then
	echo "error: data/install.nsi missing (configure should have generated it)" >&2
	exit 1
fi

# makensis is a native Windows binary and cannot read the MSYS2 POSIX paths
# that configure substitutes for @srcdir@/@top_srcdir@ in install.nsi
# (e.g. '!include "/d/a/.../data\process_running.nsh"'). Rewrite them to
# Windows form. Replace the longer srcdir prefix first.
sed -i \
	-e "s|$SRC/data|$(cygpath -w "$SRC/data" | sed 's#\\#/#g')|g" \
	-e "s|$SRC|$(cygpath -w "$SRC" | sed 's#\\#/#g')|g" \
	data/install.nsi

makensis data/install.nsi

SETUP="$(find . -maxdepth 3 \( -name 'FileZilla_*setup*.exe' -o -name 'FileZilla_3_setup.exe' \) -print -quit)"
if [[ -z "$SETUP" || ! -f "$SETUP" ]]; then
	echo "error: NSIS setup executable not found after makensis" >&2
	exit 1
fi

VERSION="$(sed -n 's/^AC_INIT(\[FileZilla\],\[\([^]]*\)\].*/\1/p' "$SRC/configure.ac")"
ARCH_TAG="win64"
if echo "${MSYSTEM:-}" | grep -qiE 'mingw32|i686'; then
	ARCH_TAG="win32"
fi

OUT_NAME="FileZilla_${VERSION}_${ARCH_TAG}-setup.exe"
cp "$SETUP" "$OUT/$OUT_NAME"
log "Installer: $OUT/$OUT_NAME"

# Portable zip via generated makezip.sh when present
if [[ -x data/makezip.sh ]]; then
	log "Creating portable zip"
	make install DESTDIR="$BUILD/install-tree"
	if data/makezip.sh "$BUILD/install-tree$PREFIX"; then
		ZIPDIR="win32zip"
		[[ -d data/win32zip ]] && ZIPDIR="data/win32zip"
		if [[ -d "$ZIPDIR" ]]; then
			if command -v zip >/dev/null 2>&1; then
				(cd "$ZIPDIR" && zip -r "$OUT/FileZilla_${VERSION}_${ARCH_TAG}-portable.zip" .)
			else
				tar -C "$ZIPDIR" -a -cf "$OUT/FileZilla_${VERSION}_${ARCH_TAG}-portable.zip" .
			fi
		fi
	fi
fi

if [[ -f src/interface/.libs/filezilla.exe ]]; then
	cp src/interface/.libs/filezilla.exe "$OUT/filezilla.exe"
elif [[ -f src/interface/filezilla.exe ]]; then
	cp src/interface/filezilla.exe "$OUT/filezilla.exe"
fi

log "Artifacts in $OUT:"
ls -la "$OUT"
popd >/dev/null
