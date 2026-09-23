#!/usr/bin/env bash
# Download (if needed) and build libfilezilla + libfzssh.
# Prefer vendored tarballs in deps/ so CI does not depend on filezilla-project.org.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/prefix}"
SRC_ROOT="${SRC_ROOT:-$HOME/deps-src}"
WORKERS="${WORKERS:-$(nproc 2>/dev/null || echo 2)}"

LIBFILEZILLA_VERSION="${LIBFILEZILLA_VERSION:-0.57.0}"
FZSSH_VERSION="${FZSSH_VERSION:-1.4.0}"

# Repo-relative deps dir (works from workspace root or scripts/ci)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEPS_DIR="${DEPS_DIR:-$REPO_ROOT/deps}"

mkdir -p "$PREFIX" "$SRC_ROOT" "$DEPS_DIR"
export PATH="$PREFIX/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}"
export LC_ALL=C

# IMPORTANT: log to stderr so command substitutions only capture real return values.
log()  { echo "==> $*" >&2; }
fail() { echo "error: $*" >&2; exit 1; }

curl_fetch() {
	curl -fsSL --retry 3 --retry-delay 2 \
		-A "Mozilla/5.0 (compatible; filezilla-ci/1.0)" \
		"$@"
}

# Resolve tarball path: prefer deps/, else download via download.php scrape.
resolve_tarball() {
	local name="$1" version="$2" page="$3"
	local tarball="${name}-${version}.tar.xz"
	local dest="$DEPS_DIR/$tarball"

	if [[ -f "$dest" && -s "$dest" ]]; then
		log "Using vendored $tarball"
		printf '%s\n' "$dest"
		return 0
	fi

	# Also accept a previously downloaded copy in SRC_ROOT
	if [[ -f "$SRC_ROOT/$tarball" && -s "$SRC_ROOT/$tarball" ]]; then
		log "Using cached $tarball"
		printf '%s\n' "$SRC_ROOT/$tarball"
		return 0
	fi

	log "Fetching download page for $name $version ($page)"
	local html url
	if ! html="$(curl_fetch "$page")"; then
		fail "cannot fetch $page (network or bot-block). Place $tarball in deps/ and retry."
	fi

	url="$(printf '%s\n' "$html" | grep -oE 'https://[^"]+/'"$tarball"'[^"]*' | head -n1 || true)"
	if [[ -z "$url" ]]; then
		fail "no download link for $tarball on $page (page layout changed or blocked). Place $tarball in deps/ and retry."
	fi

	log "Downloading $url"
	if ! curl_fetch -o "$dest" "$url"; then
		rm -f "$dest"
		fail "download of $tarball failed. Place $tarball in deps/ and retry."
	fi

	printf '%s\n' "$dest"
}

build_autotools_dep() {
	local tarball="$1" name="$2" version="$3"
	local srcdir="$SRC_ROOT/${name}-${version}"

	if [[ ! -d "$srcdir" ]]; then
		log "Extracting $(basename "$tarball")"
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

	if [[ ! -d "$srcdir" ]]; then
		log "Extracting $(basename "$tarball")"
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
	log "PREFIX=$PREFIX"
	log "SRC_ROOT=$SRC_ROOT"
	log "DEPS_DIR=$DEPS_DIR"
	log "WORKERS=$WORKERS"

	local lfz_tb fzssh_tb

	lfz_tb="$(resolve_tarball libfilezilla "$LIBFILEZILLA_VERSION" \
		"https://lib.filezilla-project.org/download.php")"
	build_autotools_dep "$lfz_tb" libfilezilla "$LIBFILEZILLA_VERSION"

	fzssh_tb="$(resolve_tarball fzssh "$FZSSH_VERSION" \
		"https://fzssh.filezilla-project.org/download.php")"
	build_meson_dep "$fzssh_tb" fzssh "$FZSSH_VERSION"

	log "Dependencies installed under $PREFIX"
	pkg-config --modversion libfilezilla >&2
	pkg-config --modversion libfzssh-client >&2
}

main "$@"
