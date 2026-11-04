![](1snap.png)

### 0verbs

Search OpenBSD and NetBSD package lists from a local cache.

`0verbs` downloads the package index for a release once, caches it, and then
answers searches without touching the network. It understands the different
index formats used by the two projects:

* OpenBSD: `SHA256` files with base64 digests.
* NetBSD: `SHA512.bz2` files with hex digests.

## Build

Requirements: a C++17 compiler, libcurl, and libbz2.

```sh
make -j8
sudo make install
```

On OpenBSD the dependencies are available as `curl` and `bzip2`; on NetBSD
install `curl` and `bzip2` from pkgsrc. A direct build is:

```sh
c++ -O2 -std=c++17 -o 0verbs 0verbs.cpp -lcurl -lbz2
```

## Usage

```text
0verbs [options] [package ...]
```

Every search word must occur in a package name. Words are case-insensitive.
A word containing regex punctuation (`^$*?()[]{}|\`) is treated as a regular
expression unless `-F` is given.

Common examples:

```sh
0verbs firefox                     # search the current release
0verbs -e php --newest             # exact package, newest patch of each line
0verbs --os netbsd --json jdk      # machine-readable NetBSD results
0verbs --list-releases             # show releases available on the mirror
0verbs --sha256 --url firefox      # include digest and download URL
0verbs --installed firefox         # mark locally installed packages
0verbs --update                    # refresh the cached indexes
0verbs --clean                     # remove the cache
```

### Options

| Option | Meaning |
| --- | --- |
| `--os {openbsd,netbsd}` | Package universe to search (default: auto-detect). |
| `--release RELEASE` | Release to search, e.g. `7.8`, `snapshots`, `10.0_2026Q3`. |
| `--arch ARCH` | Package architecture, e.g. `amd64` or `x86_64`. |
| `--list-releases` | List releases found on the mirror. |
| `-u`, `--update` | Re-download the package indexes. |
| `--offline` | Use only cached indexes; never fetch. |
| `--mirror URL` | Override the package mirror. |
| `--cache-dir DIR` | Override the cache directory. |
| `--clean` | Remove the cache and exit. |
| `--sha256`, `--sha512` | Show package digests (SHA256 on OpenBSD, SHA512 on NetBSD). |
| `--url` | Show package download URLs. |
| `-j`, `--json` | Print machine-readable JSON. |
| `--limit N` | Show at most `N` results. |
| `--installed` | Mark packages installed on this machine. |
| `--installed-only` | Show only installed packages. |
| `--newest` | Show only the newest version of each package line. |
| `--max-age DAYS` | Warn when the cached index is older than `DAYS` (default 7; 0 disables). |
| `--timeout SECONDS` | Network timeout (default 120). |
| `-e`, `--exact` | Match whole package names/stems. |
| `-F`, `--fixed-strings` | Treat search words as literal strings. |
| `--no-suggest` | Disable “Did you mean?” suggestions. |
| `-v`, `--verbose` | Print extra diagnostics. |
| `-h`, `--help` | Show help. |
| `--version` | Show the version. |

## Cache

The default cache directory is `$XDG_CACHE_HOME/0verbs` or
`~/.cache/0verbs`. Layout:

```text
0verbs/openbsd/<release>/<packages|packages-stable>/<arch>/SHA256
0verbs/netbsd/<release>/packages/<arch>/SHA512.bz2
0verbs/netbsd/<release>/packages/<arch>/SHA512
```

`--clean` refuses to delete a directory unless it is the configured cache
directory, lives beneath the default cache root, or is named `0verbs`.

## JSON output

`--json` writes an object to standard output:

```json
{
  "os": "openbsd",
  "release": "7.8",
  "arch": "amd64",
  "count": 1,
  "packages": [
    {
      "name": "firefox-149.0",
      "version": "149.0",
      "flavor": null,
      "hash_type": "sha256",
      "hash": "...",
      "url": "https://cdn.openbsd.org/pub/OpenBSD/7.8/packages/amd64/firefox-149.0.tgz",
      "branch": null,
      "installed": false
    }
  ]
}
```

`--list-releases --json` prints a JSON array of release names. Progress and
diagnostic messages are written to standard error, so stdout stays valid JSON.

## Notes

* OpenBSD SHA256 files store base64 digests; older hex-only parsing rejected
  them. This version accepts both forms.
* NetBSD architecture `amd64` is normalised to `x86_64`, and release names may
  contain an uppercase quarter such as `10.0_2026Q3`.
* Searches combine the OpenBSD `packages` and `packages-stable` indexes and
  prefer the stable entry when the same package line appears in both.
* Use `--offline` when the network is unavailable and the cache is populated.


```bash
###
# Choosing between OpenBSD and NetBSD
###
python3 0verbs.py --os openbsd
python3 0verbs.py --os netbsd

python3 0verbs.py curl

# searches are case-insensitive
python3 0verbs.py LibreOffice

# narrow the search with several words (a package must contain all of them)
python3 0verbs.py php curl

# or list packages containing either word (a regular expression, see below)
python3 0verbs.py 'php|perl'

# a word with regular expression syntax (any of ^$*?()[]{}|\) is a regex, like pacman -Ss
python3 0verbs.py '^curl-.*'      # names starting with curl-, so not curlftpfs
python3 0verbs.py '^(php|perl)'   # names starting with php or perl
python3 0verbs.py '^php.*curl'    # names starting with php, with curl later on
python3 0verbs.py '^py3-.*yaml'   # Python 3 modules about yaml
python3 0verbs.py 'gtk\+'         # a plain +, as in gtk+; \ makes + ordinary

# to list the package name and sha256 sum, sha256 is meant for OpenBSD
python3 0verbs.py --sha256 curl
python3 0verbs.py --os netbsd --sha512 '^php'

# to list the exact package, newest version: curl, but not curl-doc or libcurl
python3 0verbs.py -e curl            # or --exact curl

# several exact packages at once
python3 0verbs.py -e curl wget

# the same package with every version on the mirror, as a regular expression
python3 0verbs.py '^curl-[0-9]'

# re-download the package lists (e.g. after upgrading OpenBSD, or for new security updates)
python3 0verbs.py --update

# to list all of the available releases
python3 0verbs.py --list-releases

# or use the slow option
pkg_info -Q curl
```

---

### The C++ version needs some packages to be installed:

```bash
Build g++ -std=c++17 -O2 -o 0verbs 0verbs.cpp -lcurl -lbz2

Debian sudo apt install build-essential libcurl4-openssl-dev libbz2-dev

Fedora/RHEL sudo dnf install gcc-c++ libcurl-devel bzip2-devel

OpenBSD doas pkg_add curl bzip2

NetBSD pkgin install curl bzip2

# ---------
MacOS brew install curl bzip2
MacOS g++ -std=c++17 -O2 -o 0verbs 0verbs.cpp I$(brew --prefix curl)/include -I$(brew --prefix bzip2)/include -L$(brew --prefix curl)/lib -L$(brew --prefix bzip2)/lib -lcurl -lbz2
```

To compile the `0verbs.cpp`:

```bash
make -j8 # 8 cores/threads to use in parallel compile
sudo/doas make install
```

### The syntax from the python version stays as is with the C++ version one

---

### Trying it from macOS/Linux/other BSD

Outside OpenBSD/NetbSD the script downloads the latest release and maps your CPU to the matching OpenBSD/NetBSD architecture (Apple Silicon -> `aarch64`, Intel -> `amd64`). Pick another release or architecture with `--release` and `--arch`:

```bash
python3 0verbs.py --os openbsd --release 7.8 --arch amd64 'curl'
```

Like `pkg_add`, the script also downloads `packages-stable`, where a release's security and bug-fix updates go, and labels those results `(stable)`. As `pkg_add` would install the update, a release package that has one is left out, so `curl` shows `curl-8.19.0 (stable)` but not `curl-8.16.0`. The `packages-stable` list keeps growing, so the script reminds you to run `--update` once it is more than a week old. Each download is kept under `~/.cache/0verbs/<release>/<folder>/<arch>/SHA256`, the same path it has on the mirror, so switching between releases doesn't download again. Without `--release`/`--arch`, searches use the list you downloaded most recently (on OpenBSD, always the installed release). Older versions saved a single `~/.package_list`, which you can delete.

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