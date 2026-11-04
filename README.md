![](1snap.png)

### 0verbs

Download (local copy of the mirror's package list) and search for a OpenBSD/NetBSD package. No need to use the package searching (or set `PKG_PATH`) as it takes too long to list and find your query and you are required to have internet up and running. The program is not making a database from local ports tree programs. It creates database for the pre-compiled binaries that you will use with `pkg_add` to install the particular program that you have searched with `0verbs`.

Download this fellow and run it with the package you want. The first run downloads the mirror's small `SHA256` or `SHA512`/when on NetBSD package list, then searches it:

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

MacOS brew install curl bzip2
MacOS g++ -std=c++17 -O2 -o 0verbs 0verbs.cpp I$(brew --prefix curl)/include -I$(brew --prefix bzip2)/include -L$(brew --prefix curl)/lib -L$(brew --prefix bzip2)/lib -lcurl -lbz2
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
