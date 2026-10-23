![](1snap.png)

### 0verbs

Download (local .txt database) and search for a OpenBSD package. No need to use the package searching (or set `PKG_PATH`) as it takes too long to list and find your query and you are required to have internet up and running. The program is not making .txt database from local ports tree programs. It creates database for the pre-compiled binaries that you will use with `pkg_add` to install the particular program that you have searched with `0verbs`.

Download this fellow and run it with the package you want. The first run downloads a small `.txt` database, then searches it:

```bash
python3 0verbs.py 'curl'

# searches are case-insensitive
python3 0verbs.py 'LibreOffice'

# re-download the database (e.g. after upgrading OpenBSD)
python3 0verbs.py --update

# or use the slow option
pkg_info -aQ 'curl'
```

---

### Trying it from macOS/Linux/other BSD

Outside OpenBSD the script downloads the latest release and maps your CPU to the matching OpenBSD architecture (Apple Silicon -> `aarch64`, Intel -> `amd64`). Pick another release or architecture with `--release` and `--arch`:

```bash
python3 0verbs.py --release 7.8 --arch amd64 'curl'
```

---

### pkg_locate/locate are better ?

If you plan to use `0verbs` it's because it creates database for the pre-compiled binaries that you will install with `pkg_add`. It **doesn't** traverses `the local ports tree` like pkg_locate/locate does. It doesn't replaces your regular local tree port searches with `Makefile(s)`. It's up to you what you would like to use - ports vs pre-compiled binaries :)