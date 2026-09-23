#!/usr/bin/env bash
# Build FileZilla for Linux and package a relocatable tarball.
#
# Required environment:
#   PREFIX     - dependency install root (libfilezilla, fzssh)
#   SOURCE_DIR - FileZilla source tree (default: filezilla3)
#   OUT_DIR    - artifact output directory (default: dist)
set -euo pipefail

PREFIX="${PREFIX:-$HOME/prefix}"
SOURCE_DIR="${SOURCE_DIR:-filezilla3}"
OUT_DIR="${OUT_DIR:-dist}"
WORKERS="${WORKERS:-$(nproc 2>/dev/null || echo 2)}"

export PATH="$PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib64:${LD_LIBRARY_PATH:-}"
export LC_ALL=C

log() { echo "==> $*"; }

SRC="$(cd "$SOURCE_DIR" && pwd)"
BUILD="$SRC/compile"
OUT="$(mkdir -p "$OUT_DIR" && cd "$OUT_DIR" && pwd)"

log "Running autoreconf in $SRC"
pushd "$SRC" >/dev/null
autoreconf -fi
popd >/dev/null

log "Configuring FileZilla"
rm -rf "$BUILD"
mkdir -p "$BUILD"
pushd "$BUILD" >/dev/null
"$SRC/configure" \
	--prefix=/usr \
	--enable-shared \
	--disable-static \
	--with-pugixml=builtin \
	--without-dbus \
	--disable-shellext

log "Compiling FileZilla (-j$WORKERS)"
make -j"$WORKERS"

log "Installing into staging tree"
STAGE="$BUILD/stage"
rm -rf "$STAGE"
make install DESTDIR="$STAGE"

VERSION="$(sed -n 's/^AC_INIT(\[FileZilla\],\[\([^]]*\)\].*/\1/p' "$SRC/configure.ac")"
TARBALL="FileZilla_${VERSION}_linux-x86_64.tar.xz"

log "Creating $TARBALL"
tar -C "$STAGE" -cJf "$OUT/$TARBALL" usr

# Convenience: a small build-info file
{
	echo "FileZilla $VERSION"
	echo "Built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo "Host: $(uname -a)"
	echo "Commit: ${GITHUB_SHA:-unknown}"
} > "$OUT/build-info.txt"

log "Artifacts in $OUT:"
ls -la "$OUT"
popd >/dev/null
