#!/usr/bin/env python3
"""Monotonic channel pointer policy, used by the serialized Release publisher.
Supports format=1 (legacy stable) and format=2 (dual-channel stable/dev).
Reject same-version mutation and cross-channel pollution; permit historical
releases without downgrading a channel pointer. Fail closed on malformed input.
"""
from pathlib import Path
import re
import sys

V1_FIELDS = {
    'format', 'version', 'tag',
    'windows_url', 'windows_sha256',
    'macos_url', 'macos_sha256',
    'notes_url',
}
V2_REQUIRED = V1_FIELDS | {'channel', 'ctrl_proto', 'xfer_proto'}
V2_OPTIONAL = {'summary'}
ALL_FIELDS = V2_REQUIRED | V2_OPTIONAL
BASE = 'https://github.com/alex520lq/HopDesk-Releases/releases/'
VERSION_RE = re.compile(
    r'^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-dev\.([1-9][0-9]*))?$'
)


def parse_version_tuple(version_str):
    m = VERSION_RE.fullmatch(version_str)
    if not m:
        raise ValueError('Invalid version')
    major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3))
    dev_str = m.group(4)
    if dev_str is not None:
        dev_num = int(dev_str)
        if any(n > 4294967295 for n in (major, minor, patch, dev_num)):
            raise ValueError('Version component overflow')
        # SemVer precedence: X.Y.Z-dev.N < X.Y.Z
        return (major, minor, patch, 0, dev_num), 'dev'
    if any(n > 4294967295 for n in (major, minor, patch)):
        raise ValueError('Version component overflow')
    return (major, minor, patch, 1, 0), 'stable'


def parse(text):
    if not text or len(text.encode('utf-8')) > 8192 or '\0' in text:
        raise ValueError('Invalid manifest size/content')
    data = {}
    for line in text.splitlines():
        if not line:
            continue
        key, sep, value = line.partition('=')
        if not sep or key not in ALL_FIELDS or key in data or not value:
            raise ValueError('Unknown, duplicate or invalid manifest field')
        if key == 'summary':
            if len(value.encode('utf-8')) >= 160 or any(ord(c) < 32 or ord(c) == 127 for c in value):
                raise ValueError('Invalid summary field')
        else:
            if any(not 33 <= ord(c) <= 126 for c in value):
                raise ValueError('Invalid ASCII manifest field')
        data[key] = value

    fmt = data.get('format')
    if fmt == '1':
        if set(data) != V1_FIELDS:
            raise ValueError('Missing fields or unexpected format=1 keys')
        channel = 'stable'
    elif fmt == '2':
        if not V2_REQUIRED.issubset(data) or not set(data).issubset(ALL_FIELDS):
            raise ValueError('Missing fields or unexpected format=2 keys')
        channel = data['channel']
        if channel not in ('stable', 'dev'):
            raise ValueError('Invalid channel')
        for proto_key in ('ctrl_proto', 'xfer_proto'):
            if not re.fullmatch(r'[1-9][0-9]*', data[proto_key]):
                raise ValueError(f'Invalid {proto_key}')
            if int(data[proto_key]) > 65535:
                raise ValueError(f'{proto_key} overflow')
    else:
        raise ValueError('Unsupported manifest format')

    version, version_channel = parse_version_tuple(data['version'])
    if version_channel != channel:
        raise ValueError('Version/channel mismatch')

    tag = 'v' + data['version']
    if data['tag'] != tag or data['notes_url'] != BASE + 'tag/' + tag:
        raise ValueError('Tag/version mismatch')
    for platform, asset in [('windows', 'HopDesk-Windows.zip'), ('macos', 'HopDesk-macOS.zip')]:
        if (data[platform + '_url'] != BASE + 'download/' + tag + '/' + asset or
                not re.fullmatch('[0-9a-fA-F]{64}', data[platform + '_sha256'])):
            raise ValueError('Invalid release URL/hash')
    return version, data


def promote(candidate, current):
    text = candidate.read_text(encoding='utf-8')
    version, data = parse(text)
    cand_channel = data.get('channel', 'stable')
    if current.exists():
        old_version, old_data = parse(current.read_text(encoding='utf-8'))
        old_channel = old_data.get('channel', 'stable')
        if cand_channel != old_channel:
            raise ValueError('Refusing to overwrite pointer with different channel')
        if version < old_version:
            return 'skip-older'
        if version == old_version:
            if data != old_data:
                raise ValueError('Refusing to mutate an already-published version')
            return 'unchanged'
    temp = current.with_name(current.name + '.tmp')
    temp.write_text(text, encoding='utf-8')
    temp.replace(current)
    return 'promoted'


if __name__ == '__main__':
    print(promote(Path(sys.argv[1]), Path(sys.argv[2])))
