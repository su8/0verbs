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
import os
import platform
import re
import sys
from urllib.error import HTTPError
from urllib.request import urlopen

DATABASE = os.path.join(os.path.expanduser('~'), '.package_list')
DEFAULT_RELEASE = '8.0'

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

def package_list_url():
    release = os.getenv('OPENBSD_RELEASE') or (
        platform.release() if platform.system() == 'OpenBSD' else DEFAULT_RELEASE)
    machine = platform.machine().lower()
    arch = os.getenv('OPENBSD_ARCH') or ARCH_NAMES.get(machine, machine)
    return f'https://ftp.openbsd.org/pub/OpenBSD/{release}/packages/{arch}/SHA256'

def download():
    url = package_list_url()
    print(f'Downloading {url} ...', end=' ', flush=True)
    try:
        packages = urlopen(url, timeout=120).read().decode('utf-8')
    except HTTPError as err:
        print()
        sys.exit(f'ERROR: {err} for {url}\nSet OPENBSD_RELEASE and/or OPENBSD_ARCH'
            ' to a release and architecture that exist on the mirror.')
    with open(DATABASE, 'w') as db:
        db.write(packages)
    print(f'saved to {DATABASE}')

def search(query):
    query = query.lower()
    with open(DATABASE) as db:
        for line in db:
            # SHA256 (curl-8.22.0.tgz) = <hash>
            match = re.search(r'\((.+)\.tgz\)', line)
            if match and query in match.group(1).lower():
                print(match.group(1))

def main():
    parser = argparse.ArgumentParser(
        description='Search the OpenBSD package list from a local database.')
    parser.add_argument('query', nargs='?', help='package name to search for (case-insensitive)')
    parser.add_argument('--update', action='store_true', help='re-download the package database')
    args = parser.parse_args()

    if args.update or not os.path.isfile(DATABASE):
        download()
    elif args.query is None:
        sys.exit('ERROR: Please supply some package!')

    if args.query is not None:
        search(args.query)

if __name__ == '__main__':
    main()
