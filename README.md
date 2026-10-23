![](1snap.png)

### 0verbs

Download (local .txt database) and search for OpenBSD package. No need to use the package searching (or set `PKG_PATH`) as it takes too long to list and find your query and you are required to have internet up and running.

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
