# CI build scripts for FileZilla (with high-concurrency patch)

Produces:

- **Windows x64**: `FileZilla_<ver>_win64-setup.exe` (NSIS installer)
- **Linux x86_64**: `FileZilla_<ver>_linux-x86_64.tar.xz`

## Layout

```
filezilla3/                                 # FileZilla source (patched)
filezilla3-unlimit-transfer-concurrency.patch
.github/workflows/build.yml
scripts/ci/fetch-deps.sh                    # builds libfilezilla + fzssh
scripts/ci/build-windows.sh                 # FileZilla + makensis
scripts/ci/build-linux.sh                   # FileZilla + tarball
```

## Local Windows build (MSYS2 MinGW64)

```bash
pacman -S --needed base-devel curl tar xz zip autoconf automake libtool make \
  gettext pkgconf ninja \
  mingw-w64-x86_64-meson mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-nsis \
  mingw-w64-x86_64-toolchain mingw-w64-x86_64-wxwidgets3.2-msw \
  mingw-w64-x86_64-nettle mingw-w64-x86_64-gnutls \
  mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-zlib mingw-w64-x86_64-gettext \
  mingw-w64-x86_64-python mingw-w64-x86_64-argon2 \
  mingw-w64-i686-toolchain

export PREFIX="$HOME/prefix"
bash scripts/ci/fetch-deps.sh
bash scripts/ci/build-windows.sh
# → dist/FileZilla_*_win64-setup.exe
```

Set `WITH_SHELLEXT=0` to skip the 32-bit shell extension (faster).

## Notes

- Dependency versions are pinned in the workflow env (`LIBFILEZILLA_VERSION`, `FZSSH_VERSION`).
- Build steps tee their output to `build-windows.log` / `build-linux.log`, uploaded as workflow artifacts when a job fails (see the `if: failure()` steps in the workflow) so failures are debuggable without API access.
- `deps/` vendors `libfilezilla`, `fzssh`, and `nettle` tarballs so CI is self-contained (fzssh needs nettle ≥ 3.10; Ubuntu 24.04 only has 3.9).
- Transfer concurrency limit is raised 10 → 999 (see the patch file).
