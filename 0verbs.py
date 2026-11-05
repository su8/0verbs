#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright 10/23/2026 https://github.com/su8/0verbs
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
import bz2
import os
import platform
import re
import sys
import time
from urllib.error import HTTPError
from urllib.request import urlopen
from difflib import get_close_matches

CACHE_ROOT = os.path.join(os.path.expanduser('~'), '.cache', '0verbs')

# Package name: stem-version[-flavor]
# OpenBSD: version may end in pN (pkgfix) and vN (epoch);
#          see packages-specs(7).
# NetBSD : version may also end in nbN (pkgsrc revision).
PACKAGE_NAME = re.compile(
    r'(?P<stem>.+?)-(?P<version>\d[^-]*?)(?:p(?P<p>\d+))?(?:v(?P<v>\d+))?'
    r'(?:nb(?P<nb>\d+))?'
    r'(?P<flavor>(?:-[^-]+)*)$')

REGEX_SYNTAX = set('^$*?()[]{}|\\')


# ---------------------------------------------------------------------------
# Generic helpers
# ---------------------------------------------------------------------------

def version_order(name):
    return [int(part) if part.isdigit() else part
            for part in re.split(r'(\d+)', name)]


def version_line(version):
    # firefox 149/150 split on major; php 8.3/8.4, python 3.12/3.13 on minor
    m = re.match(r'(\d+)(?:\.\d+)?', version)
    return m[0] if int(m[1]) < 10 else m[1]


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


# ---------------------------------------------------------------------------
# Backends: one class per OS. To add a BSD, subclass Backend and add it to
# BACKENDS.
# ---------------------------------------------------------------------------

class Backend:
    NAME = ''
    HASH_LABEL = ''         # digest name, also the cached file name
    HASH_RE = None          # pattern with (name, digest) groups
    RELEASE_RE = None       # pattern for release dirs in the mirror listing
    ARCH_NAMES = {}         # platform.machine() -> package arch
    FOLDERS = (('packages', ''),)  # (folder, label shown after the name)
    NEWEST_ONLY = False     # always show one row per stem

    def __init__(self, arch=None):
        self.mirror = self.default_mirror()
        machine = platform.machine().lower()
        self.arch = arch or self.ARCH_NAMES.get(machine, machine)

    def default_mirror(self):
        raise NotImplementedError

    def releases_url(self):
        raise NotImplementedError

    def url(self, release, folder):
        raise NotImplementedError

    def download(self, release, folder):
        raise NotImplementedError

    def releases(self):
        listing = fetch(self.releases_url()).decode('utf-8', errors='ignore')
        return sorted(re.findall(self.RELEASE_RE, listing), key=version_order)

    def default_release(self):
        releases = self.releases()
        if not releases:
            sys.exit(f'ERROR: could not list releases at {self.releases_url()}')
        return releases[-1]

    def path(self, release, folder='packages'):
        return os.path.join(CACHE_ROOT, self.NAME, release, folder, self.arch,
                            self.HASH_LABEL)

    def save(self, path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = f'{path}.tmp'
        with open(tmp, 'wb') as fh:
            fh.write(data)
        os.replace(tmp, path)

    def parse(self, path):
        """Yield (name, digest) from a cached list."""
        try:
            with open(path, errors='replace') as fh:
                for line in fh:
                    m = self.HASH_RE.search(line)
                    if m:
                        yield m.group(1), m.group(2)
        except FileNotFoundError:
            return


class OpenBSD(Backend):
    """<release>/<folder>/<arch>/SHA256, plain text, base64 digests.
    Like pkg_add(1), the mirror comes from installurl(5) and packages-stable
    holds a release's updates."""
    NAME = 'openbsd'
    HASH_LABEL = 'SHA256'
    # Digests are base64, so keep the trailing = padding
    HASH_RE = re.compile(r'\((.+)\.tgz\) = (\S+)')
    RELEASE_RE = re.compile(r'href="(\d+\.\d+)/"')
    ARCH_NAMES = {
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
    FOLDERS = (('packages', ''), ('packages-stable', ' (stable)'))
    INSTALLURL = '/etc/installurl'
    DEFAULT_MIRROR = 'https://cdn.openbsd.org/pub/OpenBSD'

    def default_mirror(self):
        try:
            with open(self.INSTALLURL) as fh:
                url = fh.readline().strip().rstrip('/')
        except OSError:
            url = ''
        return url or self.DEFAULT_MIRROR

    def releases_url(self):
        return f'{self.mirror}/'

    def default_release(self):
        if platform.system() == 'OpenBSD':
            return platform.release()
        return super().default_release()

    def url(self, release, folder):
        return f'{self.mirror}/{release}/{folder}/{self.arch}/SHA256'

    def download(self, release, folder):
        url = self.url(release, folder)
        required = folder == 'packages'  # packages-stable is empty until updates exist
        data = fetch(url, announce=True, missing_ok=not required)
        if not data and required:
            sys.exit(f'ERROR: empty listing for {url}')
        self.save(self.path(release, folder), data)
        print(f'saved to {self.path(release, folder)}' if data else 'no updates yet')


class NetBSD(Backend):
    """<arch>/<release>/SHA512.bz2, saved decompressed, hex digests."""
    NAME = 'netbsd'
    HASH_LABEL = 'SHA512'
    HASH_RE = re.compile(r'SHA512 \((?:All/)?(.+?)\.tgz\) = ([A-Fa-f0-9]+)')
    RELEASE_RE = re.compile(r'href="(\d+\.\d+_[^/"]+)/"')
    ARCH_NAMES = {
        'x86_64': 'amd64', 'amd64': 'amd64',
        'i386': 'i386', 'i486': 'i386', 'i586': 'i386', 'i686': 'i386', 'x86': 'i386',
        'arm64': 'aarch64', 'aarch64': 'aarch64',
        'armv7l': 'earmv7hf', 'armv7': 'earmv7hf',
        'macppc': 'macppc',
        'riscv64': 'riscv64',
        'sparc64': 'sparc64',
        'mips64': 'mips64',
    }
    NEWEST_ONLY = True
    DEFAULT_MIRROR = 'https://cdn.netbsd.org/pub/pkgsrc/packages/NetBSD'

    def default_mirror(self):
        return self.DEFAULT_MIRROR

    def releases_url(self):
        return f'{self.mirror}/{self.arch}/'

    def url(self, release, folder):
        return f'{self.mirror}/{self.arch}/{release}/SHA512.bz2'

    def download(self, release, folder):
        url = self.url(release, folder)
        data = fetch(url, announce=True)
        try:
            text = bz2.decompress(data)
        except (OSError, ValueError) as err:
            sys.exit(f'ERROR: could not decompress {url}: {err}')
        self.save(self.path(release, folder), text)
        print(f'saved to {self.path(release, folder)}')


BACKENDS = {backend.NAME: backend for backend in (OpenBSD, NetBSD)}



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


def keep_newest(matches, stem_only=False):
    """
    Collapse multiple versions of the same package.

    stem_only=False (OpenBSD): keep the newest version per (stem, flavor,
        version_line), so php-8.3 and php-8.4 stay separate but
        php-8.4.1 and php-8.4.2 collapse.

    stem_only=True (NetBSD): keep the newest version per stem, so jdk-17
        and jdk-21 collapse into one row (the newest of either).
    """
    newest = {}
    for match in matches:
        parts = PACKAGE_NAME.match(match[0])
        if not parts:
            newest[match[0]] = ((), match)
            continue
        if stem_only:
            key = parts['stem']
        else:
            key = (parts['stem'], parts['flavor'],
                   version_line(parts['version']))
        # Rank: epoch (vN) first, then version, then pN (pkgfix), then nbN
        rank = (int(parts['v'] or 0),
                version_order(parts['version']),
                int(parts['p'] or -1),
                int(parts['nb'] or -1))
        if key not in newest or rank > newest[key][0]:
            newest[key] = (rank, match)
    return [match for _, match in newest.values()]


def hide_superseded(matches):
    """Hide release packages that have an update in packages-stable.

    Most ports have one version per release, so any stable update of the
    same stem and flavor replaces it (curl-8.16.0 -> curl-8.19.0). Only when
    the release has several branches (php-8.3 and php-8.4) do they match
    per version_line.
    """
    def line(name):
        parts = PACKAGE_NAME.match(name)
        return parts and (parts['stem'], parts['flavor'],
                          version_line(parts['version']))
    branches = {}
    for name, label, _ in matches:
        if not label and line(name):
            branches.setdefault(line(name)[:2], set()).add(line(name)[2])

    def key(name):
        full = line(name)
        return full and (full if len(branches.get(full[:2], ())) > 1 else full[:2])

    updated = {key(name) for name, label, _ in matches if label}
    return [m for m in matches
            if m[1] or not key(m[0]) or key(m[0]) not in updated]


def search(sources, patterns, queries=None, show_sha=False, exact=False,
           age_paths=None, hash_label='SHA256', always_newest=False):
    """
    sources        : list of (label, iterable_of_(name, hash))
    always_newest  : collapse multiple versions of the same package even when
                     exact=False. On NetBSD this uses stem-only matching so
                     only one row per package is shown; on OpenBSD the flag
                     is not used (the backend leaves it False).
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
    if exact or always_newest:
        matches = keep_newest(matches, stem_only=always_newest)

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

def name_matching(pattern, example, lower=True):
    def check(value):
        if lower:
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
OpenBSD mirrors come from {OpenBSD.INSTALLURL} (or {OpenBSD.DEFAULT_MIRROR}).
NetBSD mirrors come from {NetBSD.DEFAULT_MIRROR}.
NetBSD lists are saved decompressed as SHA512. NetBSD
always shows only the newest version of each package.''')

    parser.add_argument('queries', nargs='*', metavar='package',
        help='words to search for, case-insensitive; a package must contain '
             'all of them. A word with any of ^$*?()[]{}|\\ is a regular '
             'expression')
    parser.add_argument('--os', choices=BACKENDS,
        help='which package universe to search (default: auto-detect)')
    parser.add_argument('--sha256', action='store_true',
        help='also show each package\'s hash (SHA256 on OpenBSD, SHA512 on NetBSD)')
    parser.add_argument('--sha512', action='store_true',
        help='same as --sha256')
    parser.add_argument('--update', action='store_true',
        help='re-download the package lists')
    parser.add_argument('--release',
        # NetBSD release dirs have an uppercase Q (11.0_2026Q3), so keep the case
        type=name_matching(r'\d+\.\d+(_[A-Za-z0-9]+)?|snapshots',
                           '7.8, snapshots or 11.0_2026Q3', lower=False),
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

def run(backend, args):
    if args.list_releases:
        print(*backend.releases(), sep='\n')
        return

    release = args.release or backend.default_release()
    downloaded = [folder for folder, _ in backend.FOLDERS
                  if args.update or not os.path.isfile(backend.path(release, folder))]
    for folder in downloaded:
        backend.download(release, folder)

    if not args.queries:
        if not downloaded:
            sys.exit('ERROR: Please supply some package!')
        return

    sources = [(label, backend.parse(backend.path(release, folder)))
               for folder, label in backend.FOLDERS]
    search(sources, [word_pattern(w) for w in args.queries],
           queries=args.queries,
           show_sha=args.sha256 or args.sha512,
           exact=args.exact,
           # the last folder is the one that changes (packages-stable on OpenBSD)
           age_paths=[backend.path(release, backend.FOLDERS[-1][0])],
           hash_label=backend.HASH_LABEL,
           always_newest=backend.NEWEST_ONLY)


def main():
    prog = os.path.basename(sys.argv[0])
    args = build_parser(prog).parse_intermixed_args()
    backend = BACKENDS[args.os or detect_os()]
    run(backend(args.arch), args)


if __name__ == '__main__':
    main()
