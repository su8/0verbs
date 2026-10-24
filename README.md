![](1snap.png)

### 0verbs

Download (local copy of the mirror's package list) and search for a OpenBSD package. No need to use the package searching (or set `PKG_PATH`) as it takes too long to list and find your query and you are required to have internet up and running. The program is not making a database from local ports tree programs. It creates database for the pre-compiled binaries that you will use with `pkg_add` to install the particular program that you have searched with `0verbs`.

Download this fellow and run it with the package you want. The first run downloads the mirror's small `SHA256` package list, then searches it:

```bash
python3 0verbs.py curl

# searches are case-insensitive
python3 0verbs.py LibreOffice

# narrow the search with several words (a package must contain all of them)
python3 0verbs.py php curl

# to list the package name and sha256 sum
python3 0verbs.py --sha256 curl

# re-download the package lists (e.g. after upgrading OpenBSD, or for new security updates)
python3 0verbs.py --update

# to list all of the available releases
python3 0verbs.py --list-releases

# or use the slow option
pkg_info -Q curl
```

---

### Trying it from macOS/Linux/other BSD

Outside OpenBSD the script downloads the latest release and maps your CPU to the matching OpenBSD architecture (Apple Silicon -> `aarch64`, Intel -> `amd64`). Pick another release or architecture with `--release` and `--arch`:

```bash
python3 0verbs.py --release 7.8 --arch amd64 'curl'
```

Like `pkg_add`, the script also downloads `packages-stable`, where a release's security and bug-fix updates go, and labels those results `(stable)`, e.g. `curl-8.19.0 (stable)` next to `curl-8.16.0`. That list keeps growing, so the script reminds you to run `--update` once it is more than a week old. Each download is kept under `~/.cache/0verbs/<release>/<folder>/<arch>/SHA256`, the same path it has on the mirror, so switching between releases doesn't download again. Without `--release`/`--arch`, searches use the list you downloaded most recently (on OpenBSD, always the installed release). Older versions saved a single `~/.package_list`, which you can delete.

### Add it to your shell configuration file:

```bash
# Specifying function named "ss"
ss() {
  python3 0verbs.py "$@"
}

# and use it
ss 'curl'
```

---

### pkg_locate/locate are better ?

If you plan to use `0verbs` it's because it creates database for the pre-compiled binaries that you will install with `pkg_add`. It **doesn't** traverses `the local ports tree` like pkg_locate/locate does. It doesn't replaces your regular local tree port searches with `Makefile(s)`. It's up to you what you would like to use - ports vs pre-compiled binaries :)
