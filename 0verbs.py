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
# The program is python2 and python3 compatibile
import argparse
import os
import platform
import re
import sys
try:
    from urllib.request import urlopen
except ImportError:
    from urllib2 import urlopen

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

class PackageFinder(object):

    def __init__(self):
        parser = argparse.ArgumentParser(
            description='Search the OpenBSD package list from a local database.')
        parser.add_argument('query', nargs='?', help='package name to search for')
        parser.add_argument('--update', action='store_true', help='re-download the package database')
        args = parser.parse_args()

        the_list = os.path.join(os.path.expanduser('~'), '.package_list')

        if args.update or not os.path.isfile(the_list):
            self.download(the_list)
        elif args.query is None:
            sys.exit('ERROR: Please supply some package!')

        if args.query is not None:
            self.search(the_list, args.query)

    def download(self, the_list):
        release, arch = self.release(), self.arch()
        ftp_url = 'https://ftp.openbsd.org/pub/OpenBSD/{release}/packages/{arch}/SHA256'\
        .format(release=release, arch=arch)

        print('Creating a small local database from all {0} packages'\
        .format(ftp_url))

        all_packages = urlopen(ftp_url, timeout=120).read().decode('utf-8')

        with open(the_list, 'wt') as z:
            z.write(all_packages)
        print('You are ready to perform local package search,'
            ' database saved in {0}'.format(the_list))

    def search(self, the_list, query):
        with open(the_list, 'rt') as read_packs:
            for x in read_packs:
                current_line = ''.join(re.findall(r'\([^()]*\)', x))\
                .replace('(', '').replace(')', '')
                if query in current_line:
                    found_match, _ = os.path.splitext(current_line)
                    print(found_match)

    def release(self):
        if os.getenv('OPENBSD_RELEASE'):
            return os.getenv('OPENBSD_RELEASE')
        if platform.system() == 'OpenBSD':
            return platform.release()
        return DEFAULT_RELEASE

    def arch(self):
        if os.getenv('OPENBSD_ARCH'):
            return os.getenv('OPENBSD_ARCH')
        machine = platform.machine().lower()
        return ARCH_NAMES.get(machine, machine)

if __name__ == '__main__':
    PackageFinder()
