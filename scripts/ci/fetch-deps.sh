#!/usr/bin/env bash
# Download (if needed) and build nettle, libfilezilla and libfzssh into PREFIX.
# Prefer vendored tarballs in deps/ so CI does not depend on filezilla-project.org.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/prefix}"
SRC_ROOT="${SRC_ROOT:-$HOME/deps-src}"
WORKERS="${WORKERS:-$(nproc 2>/dev/null || echo 2)}"

LIBFILEZILLA_VERSION="${LIBFILEZILLA_VERSION:-0.57.0}"
FZSSH_VERSION="${FZSSH_VERSION:-1.4.0}"
NETTLE_VERSION="${NETTLE_VERSION:-3.10}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEPS_DIR="${DEPS_DIR:-$REPO_ROOT/deps}"

mkdir -p "$PREFIX" "$SRC_ROOT" "$DEPS_DIR"
# On MSYS2 MinGW, meson must run under MinGW python, not MSYS python.
if [[ -d /mingw64/bin ]]; then
	export PATH="/mingw64/bin:$PATH"
fi
export PATH="$PREFIX/bin:$PATH"
# nettle may install into lib64; always search both so pkg-config prefers PREFIX.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib64:${LD_LIBRARY_PATH:-}"
export LC_ALL=C

# IMPORTANT: log to stderr so command substitutions only capture real return values.
log()  { echo "==> $*" >&2; }
fail() { echo "error: $*" >&2; exit 1; }

curl_fetch() {
	curl -fsSL --retry 3 --retry-delay 2 \
		-A "Mozilla/5.0 (compatible; filezilla-ci/1.0)" \
		"$@"
}

# $1 name, $2 version, $3 filename, $4 optional download URL
resolve_tarball() {
	local name="$1" version="$2" filename="$3" url="${4:-}"
	local dest="$DEPS_DIR/$filename"

	if [[ -f "$dest" && -s "$dest" ]]; then
		log "Using vendored $filename"
		printf '%s\n' "$dest"
		return 0
	fi
	if [[ -f "$SRC_ROOT/$filename" && -s "$SRC_ROOT/$filename" ]]; then
		log "Using cached $filename"
		printf '%s\n' "$SRC_ROOT/$filename"
		return 0
	fi

	if [[ -n "$url" ]]; then
		log "Downloading $url"
		if curl_fetch -o "$dest" "$url"; then
			printf '%s\n' "$dest"
			return 0
		fi
		rm -f "$dest"
	fi

	fail "missing $filename (expected in deps/). Download it and place it under deps/."
}

need_system_nettle_upgrade() {
	# fzssh meson requires nettle >= 3.10
	local ver
	ver="$(pkg-config --modversion nettle 2>/dev/null || echo 0)"
	log "System/prefix nettle version: ${ver:-none}"
	pkg-config --exists 'nettle >= 3.10' 2>/dev/null && return 1
	return 0
}

build_nettle() {
	local tarball="$1" version="$2"
	local srcdir="$SRC_ROOT/nettle-${version}"

	if [[ ! -d "$srcdir" ]]; then
		log "Extracting $(basename "$tarball")"
		if [[ "$tarball" == *.tar.gz ]]; then
			tar -C "$SRC_ROOT" -xzf "$tarball"
		else
			tar -C "$SRC_ROOT" -xf "$tarball"
		fi
	fi

	log "Building nettle $version"
	pushd "$srcdir" >/dev/null
	./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" --enable-shared --disable-static
	make -j"$WORKERS"
	make install
	popd >/dev/null
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
	./configure --prefix="$PREFIX" --libdir="$PREFIX/lib" --enable-shared --disable-static
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
	local meson_cmd=(meson)
	# On MSYS2 MinGW, force meson to run under MinGW python (msys python is rejected).
	if [[ -x /mingw64/bin/python3 ]]; then
		local meson_py
		meson_py="$(command -v meson || true)"
		if [[ -n "$meson_py" ]]; then
			meson_cmd=(/mingw64/bin/python3 "$meson_py")
		fi
	fi
	"${meson_cmd[@]}" setup build \
		--prefix="$PREFIX" \
		--libdir=lib \
		--buildtype=release \
		--default-library=shared
	"${meson_cmd[@]}" compile -C build -j "$WORKERS"
	"${meson_cmd[@]}" install -C build
	popd >/dev/null
}

main() {
	log "PREFIX=$PREFIX"
	log "SRC_ROOT=$SRC_ROOT"
	log "DEPS_DIR=$DEPS_DIR"
	log "WORKERS=$WORKERS"

	# Nettle >= 3.10 is required by fzssh; Ubuntu 24.04 only ships 3.9.x.
	if need_system_nettle_upgrade; then
		local nettle_tb
		nettle_tb="$(resolve_tarball nettle "$NETTLE_VERSION" "nettle-${NETTLE_VERSION}.tar.gz" \
			"https://ftp.gnu.org/gnu/nettle/nettle-${NETTLE_VERSION}.tar.gz")"
		build_nettle "$nettle_tb" "$NETTLE_VERSION"
	else
		log "nettle >= 3.10 already available, skipping source build"
	fi

	local lfz_tb fzssh_tb

	lfz_tb="$(resolve_tarball libfilezilla "$LIBFILEZILLA_VERSION" "libfilezilla-${LIBFILEZILLA_VERSION}.tar.xz")"
	build_autotools_dep "$lfz_tb" libfilezilla "$LIBFILEZILLA_VERSION"

	fzssh_tb="$(resolve_tarball fzssh "$FZSSH_VERSION" "fzssh-${FZSSH_VERSION}.tar.xz")"
	build_meson_dep "$fzssh_tb" fzssh "$FZSSH_VERSION"

	log "Dependencies installed under $PREFIX"
	pkg-config --modversion nettle >&2
	pkg-config --modversion libfilezilla >&2
	pkg-config --modversion libfzssh-client >&2
}

main "$@"
