#!/usr/bin/env bash
# Download and build libfilezilla + libfzssh (not packaged in MSYS2).
# Expects PREFIX to be set (install root). Installs with shared libs.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/prefix}"
SRC_ROOT="${SRC_ROOT:-$HOME/deps-src}"
WORKERS="${WORKERS:-$(nproc 2>/dev/null || echo 2)}"

LIBFILEZILLA_VERSION="${LIBFILEZILLA_VERSION:-0.57.0}"
FZSSH_VERSION="${FZSSH_VERSION:-1.4.0}"

mkdir -p "$PREFIX" "$SRC_ROOT"
export PATH="$PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}"
export LC_ALL=C

log() { echo "==> $*"; }

# Official CDN links carry short-lived tokens. Scrape the download page for a live URL.
fetch_tarball() {
	local name="$1" version="$2" page="$3"
	local tarball="${name}-${version}.tar.xz"
	local dest="$SRC_ROOT/$tarball"

	if [[ -f "$dest" ]]; then
		log "Using cached $tarball"
		echo "$dest"
		return 0
	fi

	log "Fetching download page for $name $version"
	local html url
	html="$(curl -fsSL --retry 3 --retry-delay 2 "$page")"
	url="$(printf '%s\n' "$html" | grep -oE 'https://[^"]+/'"$tarball"'[^"]*' | head -n1 || true)"
	if [[ -z "$url" ]]; then
		# Fallback: untokened CDN path (may redirect or 403)
		local host="dl4.cdn.filezilla-project.org"
		[[ "$name" == "fzssh" ]] && host="dl2.cdn.filezilla-project.org"
		url="https://${host}/${name}/${tarball}"
		log "Download page had no link; trying $url"
	fi

	curl -fL --retry 3 --retry-delay 2 -o "$dest" "$url"
	echo "$dest"
}

build_autotools_dep() {
	local tarball="$1" name="$2" version="$3"
	local srcdir="$SRC_ROOT/${name}-${version}"

	if [[ ! -d "$srcdir" ]]; then
		log "Extracting $tarball"
		tar -C "$SRC_ROOT" -xf "$tarball"
	fi

	log "Building $name $version"
	pushd "$srcdir" >/dev/null
	if [[ -x ./autogen.sh && ! -f ./configure ]]; then
		./autogen.sh
	elif [[ ! -f ./configure ]]; then
		autoreconf -fi
	fi
	./configure --prefix="$PREFIX" --enable-shared --disable-static
	make -j"$WORKERS"
	make install
	popd >/dev/null
}

build_meson_dep() {
	local tarball="$1" name="$2" version="$3"
	local srcdir="$SRC_ROOT/${name}-${version}"
	local builddir="$srcdir/build"

	if [[ ! -d "$srcdir" ]]; then
		log "Extracting $tarball"
		tar -C "$SRC_ROOT" -xf "$tarball"
	fi

	log "Building $name $version (meson)"
	pushd "$srcdir" >/dev/null
	meson setup build \
		--prefix="$PREFIX" \
		--libdir=lib \
		--buildtype=release \
		--default-library=shared
	meson compile -C build -j "$WORKERS"
	meson install -C build
	popd >/dev/null
}

main() {
	local lfz_tb fzssh_tb

	lfz_tb="$(fetch_tarball libfilezilla "$LIBFILEZILLA_VERSION" \
		"https://lib.filezilla-project.org/download.php")"
	build_autotools_dep "$lfz_tb" libfilezilla "$LIBFILEZILLA_VERSION"

	fzssh_tb="$(fetch_tarball fzssh "$FZSSH_VERSION" \
		"https://fzssh.filezilla-project.org/download.php")"
	build_meson_dep "$fzssh_tb" fzssh "$FZSSH_VERSION"

	log "Dependencies installed under $PREFIX"
	pkg-config --modversion libfilezilla
	pkg-config --modversion libfzssh-client
}

main "$@"
