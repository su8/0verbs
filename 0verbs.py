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
import glob
import os
import platform
import re
import sys
from urllib.error import HTTPError
from urllib.request import urlopen

# Each download is kept as <release>/packages/<arch>/SHA256, the same path as on the mirror
CACHE = os.path.join(os.path.expanduser('~'), '.cache', '0verbs')
INSTALLURL = '/etc/installurl'
DEFAULT_MIRROR = 'https://cdn.openbsd.org/pub/OpenBSD'

def mirror():
    # Use the same mirror as pkg_add(1): the first line of installurl(5)
    try:
        with open(INSTALLURL) as installurl:
            url = installurl.readline().strip().rstrip('/')
    except OSError:
        url = ''
    return url or DEFAULT_MIRROR

MIRROR = mirror()

# platform.machine() names (Linux, macOS and OpenBSD's `uname -m`)
# mapped to OpenBSD package architectures, i.e. what `machine -a` prints
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

def version_order(name):
    # Compare digit runs as numbers, so 10.0 sorts after 7.8
    return [int(part) if part.isdigit() else part for part in re.split(r'(\d+)', name)]

def fetch(url, announce=False):
    if announce:
        print(f'Downloading {url} ...', end=' ', flush=True)
    try:
        with urlopen(url, timeout=120) as response:
            return response.read().decode('utf-8')
    except HTTPError as err:
        error = f'ERROR: {err} for {url}'
        if err.code == 404:
            error += (f'\nUse --release and/or --arch to pick a release and architecture'
                f' that exist on {MIRROR}')
    except OSError as err:
        error = f'ERROR: could not download {url}: {err}'
    if announce:
        print()
    sys.exit(error)

def mirror_releases():
    listing = fetch(f'{MIRROR}/')
    return sorted(re.findall(r'href="(\d+\.\d+)/"', listing), key=version_order)

def default_release():
    # Packages only install on the release they were built for
    if platform.system() == 'OpenBSD':
        return platform.release()
    return mirror_releases()[-1]

def default_arch():
    machine = platform.machine().lower()
    return ARCH_NAMES.get(machine, machine)

def local_list(release, arch):
    return os.path.join(CACHE, release, 'packages', arch, 'SHA256')

def pick_list(release, arch, update):
    # On OpenBSD the installed release is the only one pkg_add(1) can use;
    # elsewhere prefer the newest downloaded list over asking the mirror
    if release is None and not update and platform.system() != 'OpenBSD':
        lists = sorted(glob.glob(local_list('*', arch or '*')), key=os.path.getmtime)
        if lists:
            cached_release, _, cached_arch, _ = os.path.relpath(lists[-1], CACHE).split(os.sep)
            return cached_release, cached_arch
    return release or default_release(), arch or default_arch()

def download(release, arch):
    url = f'{MIRROR}/{release}/packages/{arch}/SHA256'
    database = local_list(release, arch)
    packages = fetch(url, announce=True)
    os.makedirs(os.path.dirname(database), exist_ok=True)
    # Write a temporary file first so an interrupted run never leaves half a list
    with open(f'{database}.tmp', 'w') as db:
        db.write(packages)
    os.replace(f'{database}.tmp', database)
    print(f'saved to {database}')

def search(database, query, show_sha=False):
    query = query.lower()
    with open(database) as db:
        for line in db:
            # SHA256 (curl-8.22.0.tgz) = <base64 hash>=
            # the hash group stops before the trailing '=' padding
            match = re.search(r'\((.+)\.tgz\) = ([A-Za-z0-9+/]+)', line)
            if not match or query not in match.group(1).lower():
                continue
            if show_sha:
                print(f'Package: {match.group(1)}')
                print(f'SHA256: {match.group(2)}')
            else:
                print(match.group(1))

def name_matching(pattern, example):
    # --release and --arch become cache directories, so allow only plain names
    def check(value):
        value = value.lower()
        if not re.fullmatch(pattern, value):
            raise argparse.ArgumentTypeError(f'{value!r} is not like {example}')
        return value
    return check

def main():
    parser = argparse.ArgumentParser(
        description='Search the OpenBSD package list from a local database.')
    parser.add_argument('query', nargs='?', help='package name to search for (case-insensitive)')
    parser.add_argument('--update', action='store_true', help='re-download the package list')
    parser.add_argument('--release', type=name_matching(r'\d+\.\d+|snapshots', '7.8 or snapshots'),
        help='OpenBSD release to search, e.g. 7.8, downloaded if needed (default: the installed'
        ' release on OpenBSD; elsewhere the newest downloaded list, or the latest release)')
    parser.add_argument('--arch', type=name_matching(r'[a-z0-9]+', 'amd64'),
        help='package architecture to search, e.g. amd64, downloaded if needed'
        ' (default: this machine\'s, or the newest downloaded list\'s outside OpenBSD)')
    parser.add_argument('--list-releases', action='store_true', help='list available OpenBSD releases on the mirror')
    parser.add_argument('SHA256', nargs='?', help='Show the SHA256 sum for the particular package search')
    args = parser.parse_args()

    if args.list_releases:
        print(*mirror_releases(), sep='\n')
        return

    target = pick_list(args.release, args.arch, args.update)
    database = local_list(*target)
    if args.update or not os.path.isfile(database):
        download(*target)
    elif args.SHA256:
        search(database, args.query, show_sha=True)
        sys.exit(0)
    elif args.query is None:
        sys.exit('ERROR: Please supply some package!')

    if args.query is not None:
        search(database, args.query, show_sha=False)

if __name__ == '__main__':
    main()