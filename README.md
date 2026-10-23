![](1snap.png)

### 0verbs

Download (local .txt database) and search for OpenBSD package. No need to use the package searching (or set `PKG_PATH`) as it takes too long to list and find your query and you are required to have internet up and running.

Download this fellow and run it with the package you want. The first run downloads a small `.txt` database, then searches it:

```bash
python3 0verbs.py 'curl'

# re-download the database (e.g. after upgrading OpenBSD)
python3 0verbs.py --update

# or use the slow option
pkg_info -aQ 'curl'
```

---

### Trying it from macOS/Linux/other BSD

Outside OpenBSD the script uses the `8.0` release and maps your CPU to the matching OpenBSD architecture (Apple Silicon -> `aarch64`, Intel -> `amd64`). To pick another release or architecture, set these environment variables:

```bash
OPENBSD_RELEASE=7.8 OPENBSD_ARCH=amd64 python3 0verbs.py --update
```
