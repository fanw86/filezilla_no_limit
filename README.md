# FileZilla — Unlimited Transfer Concurrency Builds

Unofficial, automated builds of [FileZilla](https://filezilla-project.org/) with the
transfer concurrency limits raised from **10 to 999**:

- Maximum simultaneous transfers: 1–999 (stock: 1–10)
- Concurrent download limit: 0–999, 0 = no limit (stock: 0–10)
- Concurrent upload limit: 0–999, 0 = no limit (stock: 0–10)

Useful when transferring many small files to/from servers that allow many parallel
connections, where the stock cap of 10 concurrent transfers is the bottleneck.

## Downloads

Prebuilt artifacts are published as GitHub Releases (see the Releases page):

- **Windows x64**: `FileZilla_<ver>_win64-setup.exe` (NSIS installer)
- **Linux x86_64**: `FileZilla_<ver>_linux-x86_64.tar.xz`

These are built by GitHub Actions directly from the sources in this repository.

## What's patched

The only functional change relative to upstream is
[`filezilla3-unlimit-transfer-concurrency.patch`](filezilla3-unlimit-transfer-concurrency.patch),
which raises the clamp/spin-control ranges in `src/interface/Options.cpp` and
`src/interface/settings/optionspage_transfer.cpp`. Everything else (protocols,
transfer engine, UI) is untouched upstream FileZilla.

The `filezilla3/` tree is pinned to the upstream **3.71.1** release. A second patch,
`filezilla3-locales-xgettext-argv-fix.patch`, works around the Windows command-line
length limit when regenerating `locales/filezilla.pot` (build tooling only, no
behavior change).

## Building

Builds run in CI (`.github/workflows/build.yml`) on every push; tags starting with
`v` additionally create a GitHub Release with the artifacts attached.

For local build instructions and CI details, see [BUILD-CI.md](BUILD-CI.md).

## Warning

Running hundreds of concurrent transfers puts real load on the server and the
network. Many servers limit simultaneous connections per client/IP and may throttle
or ban you. Use reasonable values.

## License / attribution

FileZilla is free software by Tim Kosse and the FileZilla project, licensed under
the GNU General Public License v2 or later (see `filezilla3/COPYING`). This
repository only repackages upstream source with the patch above; no affiliation
with the FileZilla project. For the official client, visit
https://filezilla-project.org/.
