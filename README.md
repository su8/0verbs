![](1snap.png)

### 0verbs

Download (local .txt database) and search for OpenBSD package. No need to use the package searching (or set `PKG_PATH`) as it takes too long to list and find your query and you are required to have internet up and running.

Download this fellow and run it without any arguments, so it can download small `.txt` database first:

```bash
python3 0verbs.py

# to query the database
python3 0verbs.py 'curl'

# or use the slow option
pkg_info -aQ 'curl'
```

---

### Trying it from macOS/Linux/other BSD

Outside OpenBSD the script uses the `8.0` release and maps your CPU to the matching OpenBSD architecture (Apple Silicon -> `aarch64`, Intel -> `amd64`). To pick another release or architecture, set these environment variables (delete `~/.package_list` first to re-download):

```bash
OPENBSD_RELEASE=7.8 OPENBSD_ARCH=amd64 python3 0verbs.py
```
