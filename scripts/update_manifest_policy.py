#!/usr/bin/env python3
"""Monotonic latest pointer policy, used by the serialized Release publisher.
Reject same-version mutation; permit historical releases without moving latest.
No network or credentials are handled here. Fail closed for malformed manifests.
"""
from pathlib import Path
import re
import sys

FIELDS = {'format','version','tag','windows_url','windows_sha256','macos_url','macos_sha256','notes_url'}
BASE = 'https://github.com/alex520lq/HopDesk-Releases/releases/'

def parse(text):
    if not text or len(text.encode('utf-8')) > 8192 or '\0' in text:
        raise ValueError('Invalid manifest size/content')
    data = {}
    for line in text.splitlines():
        if not line:
            continue
        key, sep, value = line.partition('=')
        if not sep or key not in FIELDS or key in data or not value or any(not 33 <= ord(c) <= 126 for c in value):
            raise ValueError('Unknown, duplicate or invalid manifest field')
        data[key] = value
    if set(data) != FIELDS or data['format'] != '1':
        raise ValueError('Missing fields or unsupported format')
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', data['version']):
        raise ValueError('Invalid version')
    version = tuple(map(int, data['version'].split('.')))
    if any(n > 4294967295 for n in version):
        raise ValueError('Version component overflow')
    tag = 'v' + data['version']
    if data['tag'] != tag or data['notes_url'] != BASE+'tag/'+tag:
        raise ValueError('Tag/version mismatch')
    for platform, asset in [('windows','HopDesk-Windows.zip'),('macos','HopDesk-macOS.zip')]:
        if data[platform+'_url'] != BASE+'download/'+tag+'/'+asset or not re.fullmatch('[0-9a-fA-F]{64}',data[platform+'_sha256']):
            raise ValueError('Invalid release URL/hash')
    return version, data

def promote(candidate, current):
    text = candidate.read_text(encoding='utf-8')
    version, data = parse(text)
    if current.exists():
        old_version, old_data = parse(current.read_text(encoding='utf-8'))
        if version < old_version:
            return 'skip-older'
        if version == old_version:
            if data != old_data:
                raise ValueError('Refusing to mutate an already-published version')
            return 'unchanged'
    temp = current.with_name(current.name+'.tmp')
    temp.write_text(text, encoding='utf-8')
    temp.replace(current)
    return 'promoted'

if __name__ == '__main__':
    print(promote(Path(sys.argv[1]),Path(sys.argv[2])))
