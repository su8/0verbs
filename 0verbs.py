#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright 11/04/2026 10/23/2026 https://github.com/su8/0verbs
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
# MA 02110-1301, USA.
#
import argparse
import os
import platform
import re
import sys
import time
from urllib.error import HTTPError
from urllib.request import urlopen
from difflib import get_close_matches

CACHE_ROOT = os.path.join(os.path.expanduser('~'), '.cache', '0verbs')

# ---------------------------------------------------------------------------
# Per-OS configuration
# ---------------------------------------------------------------------------
#
# "openbsd": mirrors publish a plain-text SHA256 list per
#            <release>/<folder>/<arch>/SHA256, where folder is packages
#            or packages-stable. installurl(5) is honoured.
#
# "netbsd" : mirrors publish <arch>/<release>/SHA512.bz2, a bzip2-compressed
#            list. The raw .bz2 is kept as SHA512.bz2 and a cleaned-up
#            (no "All/" prefix) text list is written to SHA512.
#
# Both formats are parsed by search(); only the download/decompress step
# and the release discovery differ.

INSTALLURL = '/etc/installurl'
OPENBSD_DEFAULT_MIRROR = 'https://cdn.openbsd.org/pub/OpenBSD'
OPENBSD_FOLDERS = ('packages', 'packages-stable')

NETBSD_DEFAULT_MIRROR = 'https://cdn.netbsd.org/pub/pkgsrc/packages/NetBSD'

# platform.machine() -> package arch, per OS family
OPENBSD_ARCH_NAMES = {
    'x86_64': 'amd64', 'amd64': 'amd64',
    'arm64': 'aarch64', 'aarch64': 'aarch64',
    'i386': 'i386', 'i486': 'i386', 'i586': 'i386', 'i686': 'i386', 'x86': 'i386',
    'armv7l': 'arm', 'armv7': 'arm',
    'macppc': 'powerpc',
    'octeon': 'mips64',
    'loongson': 'mips64el',
    'riscv64': 'riscv64',
    'ppc64': 'powerpc64', 'powerpc64': 'powerpc64',
    'sparc64': 'sparc64',
    'mips64': 'mips64',
}
NETBSD_ARCH_NAMES = {
    'x86_64': 'amd64', 'amd64': 'amd64',
    'i386': 'i386', 'i486': 'i386', 'i586': 'i386', 'i686': 'i386', 'x86': 'i386',
    'arm64': 'aarch64', 'aarch64': 'aarch64',
    'armv7l': 'earmv7hf', 'armv7': 'earmv7hf',
    'macppc': 'macppc',
    'riscv64': 'riscv64',
    'sparc64': 'sparc64',
    'mips64': 'mips64',
}

# OpenBSD package name: stem-version[-flavor], version may end in pN (pkgfix)
# and vN (epoch); see packages-specs(7). NetBSD pkg names are usually
# stem-version.tgz with no pN/vN suffix, but the regex is harmless there.
PACKAGE_NAME = re.compile(
    r'(?P<stem>.+?)-(?P<version>\d[^-]*?)(?:p(?P<p>\d+))?(?:v(?P<v>\d+))?'
    r'(?P<flavor>(?:-[^-]+)*)$')

REGEX_SYNTAX = set('^$*?()[]{}|\\')


# ---------------------------------------------------------------------------
# Generic helpers
# ---------------------------------------------------------------------------

def version_order(name):
    return [int(part) if part.isdigit() else part
            for part in re.split(r'(\d+)', name)]


def detect_os():
    if platform.system() == 'OpenBSD':
        return 'openbsd'
    return 'netbsd'


def fetch(url, announce=False, missing_ok=False):
    if announce:
        print(f'Downloading {url} ...', end=' ', flush=True)
    try:
        with urlopen(url, timeout=120) as response:
            return response.read()
    except HTTPError as err:
        if missing_ok and err.code == 404:
            return b''
        error = f'ERROR: {err} for {url}'
        if err.code == 404:
            error += (f'\nUse --release and/or --arch to pick a release and '
                      f'architecture that exist on the mirror')
    except OSError as err:
        error = f'ERROR: could not download {url}: {err}'
    if announce:
        print()
    sys.exit(error)


def local_list(flavour, release, arch, folder='packages', filename='SHA256'):
    return os.path.join(CACHE_ROOT, flavour, release, folder, arch, filename)


# ---------------------------------------------------------------------------
# OpenBSD backend
# ---------------------------------------------------------------------------

def openbsd_mirror():
    try:
        with open(INSTALLURL) as fh:
            url = fh.readline().strip().rstrip('/')
    except OSError:
        url = ''
    return url or OPENBSD_DEFAULT_MIRROR


def openbsd_mirror_releases(mirror):
    listing = fetch(f'{mirror}/').decode('utf-8', errors='ignore')
    return sorted(re.findall(r'href="(\d+\.\d+)/"', listing), key=version_order)


def openbsd_default_release(mirror):
    if platform.system() == 'OpenBSD':
        return platform.release()
    releases = openbsd_mirror_releases(mirror)
    if not releases:
        sys.exit(f'ERROR: could not list releases at {mirror}/')
    return releases[-1]


def openbsd_default_arch():
    machine = platform.machine().lower()
    return OPENBSD_ARCH_NAMES.get(machine, machine)


def openbsd_download(mirror, release, arch, folder):
    url = f'{mirror}/{release}/{folder}/{arch}/SHA256'
    db = local_list('openbsd', release, arch, folder)
    data = fetch(url, announce=True, missing_ok=folder != 'packages')
    if not data and folder == 'packages':
        sys.exit(f'ERROR: empty listing for {url}')
    os.makedirs(os.path.dirname(db), exist_ok=True)
    tmp = f'{db}.tmp'
    with open(tmp, 'wb') as fh:
        fh.write(data)
    os.replace(tmp, db)
    print(f'saved to {db}' if data else 'no updates yet')


def openbsd_parse(db_path):
    """Yield (name, sha256) from a packages SHA256 file."""
    try:
        with open(db_path, errors='replace') as fh:
            for line in fh:
                m = re.search(r'\((.+)\.tgz\) = ([A-Za-z0-9+/]+)', line)
                if m:
                    yield m.group(1), m.group(2)
    except FileNotFoundError:
        return


# ---------------------------------------------------------------------------
# NetBSD backend
# ---------------------------------------------------------------------------

def netbsd_mirror():
    return NETBSD_DEFAULT_MIRROR


def netbsd_mirror_releases(mirror, arch):
    try:
        listing = fetch(f'{mirror}/{arch}/').decode('utf-8', errors='ignore')
    except SystemExit:
        return []
    return sorted(
        re.findall(r'href="(\d+\.\d+_[^/"]+)/"', listing), key=version_order)


def netbsd_default_release(mirror, arch):
    releases = netbsd_mirror_releases(mirror, arch)
    if not releases:
        sys.exit(f'ERROR: could not list releases for arch {arch} at '
                 f'{mirror}/{arch}/')
    return releases[-1]


def netbsd_default_arch():
    machine = platform.machine().lower()
    return NETBSD_ARCH_NAMES.get(machine, machine)


def netbsd_download(mirror, release, arch):
    url = f'{mirror}/{arch}/{release}/SHA512.bz2'
    raw_db = local_list('netbsd', release, arch, 'packages', 'SHA512.bz2')
    clean_db = local_list('netbsd', release, arch, 'packages', 'SHA512')
    data = fetch(url, announce=True)
    try:
        import bz2
        text = bz2.decompress(data)
    except ImportError:
        import subprocess
        proc = subprocess.run(['bzip2', '-dc'], input=data,
                              capture_output=True, check=True)
        text = proc.stdout
    os.makedirs(os.path.dirname(raw_db), exist_ok=True)
    tmp_raw = f'{raw_db}.tmp'
    with open(tmp_raw, 'wb') as fh:
        fh.write(data)
    os.replace(tmp_raw, raw_db)
    # Strip "All/" prefix from every package path, if present.
    out_lines = []
    for line in text.decode('utf-8', errors='replace').splitlines(True):
        # Match e.g. "SHA512 (All/curl-8.10.1.tgz) = hash"
        m = re.match(r'(SHA512 \()All/(.+\.tgz\) = .*)', line)
        if m:
            out_lines.append(m.group(1) + m.group(2) + '\n')
        else:
            out_lines.append(line)
    tmp_clean = f'{clean_db}.tmp'
    with open(tmp_clean, 'w', encoding='utf-8') as fh:
        fh.writelines(out_lines)
    os.replace(tmp_clean, clean_db)
    print(f'saved raw to {raw_db}')
    print(f'saved cleaned list to {clean_db}')


def netbsd_parse(db_path):
    """Yield (name, sha512) from a NetBSD SHA512 file (All/ already stripped)."""
    try:
        with open(db_path, errors='replace') as fh:
            for line in fh:
                m = re.search(r'SHA512 \((.+?)\.tgz\) = ([A-Fa-f0-9]+)', line)
                if m:
                    yield m.group(1), m.group(2)
    except FileNotFoundError:
        return


# ---------------------------------------------------------------------------
# Shared search / output
# ---------------------------------------------------------------------------

def word_pattern(word):
    return re.compile(word if REGEX_SYNTAX & set(word) else re.escape(word),
                      re.IGNORECASE)


def exact_name(name, patterns):
    parts = PACKAGE_NAME.match(name)
    return any(pattern.fullmatch(name) or (parts and pattern.fullmatch(parts['stem']))
               for pattern in patterns)


def keep_newest(matches):
    newest = {}
    for match in matches:
        parts = PACKAGE_NAME.match(match[0])
        if not parts:
            newest[match[0]] = ((), match)
            continue
        key = (parts['stem'], parts['flavor'],
               re.match(r'\d+', parts['version'])[0])
        rank = (int(parts['v'] or 0), version_order(parts['version']),
                int(parts['p'] or -1))
        if key not in newest or rank > newest[key][0]:
            newest[key] = (rank, match)
    return [match for _, match in newest.values()]


def hide_superseded(matches):
    def line(name):
        parts = PACKAGE_NAME.match(name)
        return parts and (parts['stem'], parts['flavor'],
                          re.match(r'\d+', parts['version'])[0])
    updated = {line(name) for name, label, _ in matches if label}
    return [m for m in matches
            if m[1] or not line(m[0]) or line(m[0]) not in updated]


def search(sources, patterns, queries=None, show_sha=False, exact=False,
           age_paths=None, hash_label='SHA256'):
    """
    sources : list of (label, iterable_of_(name, hash))
    """
    matches = []
    suggestions_pool = []
    for label, items in sources:
        for name, sha in items:
            suggestions_pool.append(name)
            hit = (exact_name(name, patterns) if exact
                   else all(p.search(name) for p in patterns))
            if hit:
                matches.append((name, label, sha))

    if not matches and queries and len(queries) == 1:
        suggestions = get_close_matches(queries[0], suggestions_pool,
                                        n=5, cutoff=0.4)
        if suggestions:
            print('\nDid you mean?')
            for pkg in suggestions:
                print(f'  {pkg}')
            return

    matches = hide_superseded(matches)
    if exact:
        matches = keep_newest(matches)

    def order(match):
        return (re.split(r'-(?=\d)', match[0], maxsplit=1)[0],
                version_order(match[0]))

    for package, label, sha in sorted(matches, key=order):
        if show_sha:
            print(f'Package: {package}{label}')
            print(f'{hash_label}: {sha}')
        else:
            print(f'{package}{label}')

    if not matches:
        print('No matches found.', file=sys.stderr)

    if age_paths:
        newest = max((os.path.getmtime(p) for p in age_paths if os.path.exists(p)),
                     default=0)
        if newest:
            days = (time.time() - newest) / 86400
            if days > 7:
                print(f'Note: the list is {days:.0f} days old, run with '
                      f'--update for newer ones', file=sys.stderr)


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def name_matching(pattern, example):
    def check(value):
        value = value.lower()
        if not re.fullmatch(pattern, value):
            raise argparse.ArgumentTypeError(f'{value!r} is not like {example}')
        return value
    return check


def build_parser(prog):
    parser = argparse.ArgumentParser(
        prog=prog,
        description='Search OpenBSD and NetBSD package lists from a local '
                    'database.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f'''examples:
  {prog} curl                     packages whose name contains "curl"
  {prog} php curl 8.4             packages whose name contains every word
  {prog} --sha256 curl            also show each package's SHA256 sum (OpenBSD)
  {prog} --sha512 curl            also show each package's SHA512 sum (NetBSD)
  {prog} -e curl                  only the curl package, newest version
  {prog} -e curl wget             the curl and the wget package
  {prog} --os netbsd --release 11.0_2026Q3 --arch amd64 curl
  {prog} --os openbsd --release 7.8 --arch amd64 curl
  {prog} --update                 re-download the package lists
  {prog} --list-releases          list releases on the mirror

Package lists are saved under {CACHE_ROOT}.
OpenBSD mirrors come from {INSTALLURL} (or {OPENBSD_DEFAULT_MIRROR}).
NetBSD mirrors come from {NETBSD_DEFAULT_MIRROR}.
NetBSD keeps the raw SHA512.bz2 and a cleaned SHA512 (no "All/" prefix).''')

    parser.add_argument('queries', nargs='*', metavar='package',
        help='words to search for, case-insensitive; a package must contain '
             'all of them. A word with any of ^$*?()[]{}|\\ is a regular '
             'expression')
    parser.add_argument('--os', choices=('openbsd', 'netbsd'),
        help='which package universe to search (default: auto-detect)')
    parser.add_argument('--sha256', action='store_true',
        help='also show each package\'s SHA256 sum (OpenBSD lists)')
    parser.add_argument('--sha512', action='store_true',
        help='also show each package\'s SHA512 sum (NetBSD lists)')
    parser.add_argument('--update', action='store_true',
        help='re-download the package lists')
    parser.add_argument('--release',
        type=name_matching(r'\d+\.\d+(_[a-z0-9q]+)?|snapshots',
                           '7.8, snapshots or 11.0_2026Q3'),
        help='release to search, downloaded if needed')
    parser.add_argument('--arch',
        type=name_matching(r'[a-z0-9]+', 'amd64'),
        help='package architecture to search, downloaded if needed')
    parser.add_argument('--list-releases', action='store_true',
        help='list available releases on the mirror')
    parser.add_argument('-e', '--exact', action='store_true',
        help='match whole package names instead of parts and show the '
             'newest version')
    return parser


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def run_openbsd(args, parser):
    mirror = openbsd_mirror()
    release = args.release or openbsd_default_release(mirror)
    arch = args.arch or openbsd_default_arch()

    if args.list_releases:
        print(*openbsd_mirror_releases(mirror), sep='\n')
        return

    missing = [folder for folder in OPENBSD_FOLDERS
               if args.update
               or not os.path.isfile(local_list('openbsd', release, arch, folder))]
    for folder in missing:
        openbsd_download(mirror, release, arch, folder)

    if args.queries:
        patterns = [word_pattern(w) for w in args.queries]
        sources = []
        for folder in OPENBSD_FOLDERS:
            label = ' (stable)' if folder == 'packages-stable' else ''
            db = local_list('openbsd', release, arch, folder)
            sources.append((label, openbsd_parse(db)))
        search(sources, patterns, queries=args.queries,
               show_sha=args.sha256,
               exact=args.exact,
               age_paths=[local_list('openbsd', release, arch, 'packages-stable')],
               hash_label='SHA256')
    elif not missing:
        sys.exit('ERROR: Please supply some package!')


def run_netbsd(args, parser):
    mirror = netbsd_mirror()
    arch = args.arch or netbsd_default_arch()

    if args.list_releases:
        print(*netbsd_mirror_releases(mirror, arch), sep='\n')
        return

    release = args.release or netbsd_default_release(mirror, arch)
    db = local_list('netbsd', release, arch, 'packages', 'SHA512')

    if args.update or not os.path.isfile(db):
        netbsd_download(mirror, release, arch)

    if args.queries:
        patterns = [word_pattern(w) for w in args.queries]
        sources = [('', netbsd_parse(db))]
        search(sources, patterns, queries=args.queries,
               show_sha=args.sha512,
               exact=args.exact,
               age_paths=[db],
               hash_label='SHA512')
    elif not args.update:
        sys.exit('ERROR: Please supply some package!')


def main():
    prog = os.path.basename(sys.argv[0])
    parser = build_parser(prog)
    args = parser.parse_intermixed_args()

    target = args.os or detect_os()
    if target == 'openbsd':
        run_openbsd(args, parser)
    else:
        run_netbsd(args, parser)


if __name__ == '__main__':
    main()