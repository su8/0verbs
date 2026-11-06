#!/usr/bin/env python3
# Run: python3 test_0verbs.py   (or: pytest test_0verbs.py)
import importlib.util
import os
import tempfile

_spec = importlib.util.spec_from_file_location(
    'overbs', os.path.join(os.path.dirname(os.path.abspath(__file__)), '0verbs.py'))
v = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(v)


def _write(text):
    with tempfile.NamedTemporaryFile('w', delete=False) as fh:
        fh.write(text)
    return fh.name


def test_lines_split_on_minor_below_10():
    m = [('php-8.3.20', '', ''), ('php-8.4.10', '', ''), ('php-8.4.11', ' (stable)', '')]
    assert [x[0] for x in v.hide_superseded(m)] == ['php-8.3.20', 'php-8.4.11']
    assert sorted(x[0] for x in v.keep_newest(m)) == ['php-8.3.20', 'php-8.4.11']


def test_stable_update_hides_single_release_version():
    m = [('curl-8.16.0', '', ''), ('curl-8.19.0', ' (stable)', '')]
    assert [x[0] for x in v.hide_superseded(m)] == ['curl-8.19.0']


def test_netbsd_newest_uses_nb_revision():
    m = [('curl-8.10.1nb2', '', ''), ('curl-8.10.1nb10', '', ''), ('curl-8.9.0', '', '')]
    assert [x[0] for x in v.keep_newest(m, stem_only=True)] == ['curl-8.10.1nb10']


def test_netbsd_newest_collapses_branch_prefix():
    m = [('php56-curl-5.6.40nb28', '', ''), ('php85-curl-8.5.12nb28', '', ''),
         ('php84-curl-8.4.27nb28', '', ''), ('py39-yaml-6.0', '', ''),
         ('py313-yaml-6.0', '', ''), ('wget-1.25', '', ''), ('wget2-2.3.0', '', '')]
    assert sorted(x[0] for x in v.keep_newest(m, stem_only=True)) == [
        'php85-curl-8.5.12nb28', 'py313-yaml-6.0', 'wget-1.25', 'wget2-2.3.0']


def test_openbsd_hash_keeps_base64_padding():
    path = _write('SHA256 (curl-8.10.1.tgz) = ab+/cd=\n')
    assert list(v.OpenBSD().parse(path)) == [('curl-8.10.1', 'ab+/cd=')]


def test_netbsd_parse_strips_all_prefix():
    path = _write('SHA512 (All/curl-8.10.1nb1.tgz) = abc123\n')
    assert list(v.NetBSD().parse(path)) == [('curl-8.10.1nb1', 'abc123')]


def test_release_keeps_case():
    args = v.build_parser('0verbs').parse_args(['--release', '11.0_2026Q3'])
    assert args.release == '11.0_2026Q3'


def test_arch_is_normalized():
    args = v.build_parser('0verbs').parse_args(['--arch', 'x86_64'])
    assert v.OpenBSD(args.arch).arch == 'amd64'
    assert v.NetBSD('armv7l').arch == 'earmv7hf'


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_'):
            fn()
    print('ok')
