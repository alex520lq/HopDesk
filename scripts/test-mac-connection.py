#!/usr/bin/env python3
"""Exercise production macOS TCP loops with simulated peers and UI effects.

Uses real loopback TCP sockets, but does not test AppKit, a physical desktop,
macOS firewall permissions, or a real Windows peer.
"""
import os, pathlib, re, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parent.parent

def extract(source, name):
    match = re.search(r'^[^\n;{}]*\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError('Missing production function: ' + name)
    start = match.start()
    i = source.index('{', start)
    depth = 1
    j = i + 1
    while depth:
        if source[j] == '{': depth += 1
        elif source[j] == '}': depth -= 1
        j += 1
    return source[start:j]

def run(work, name, source, extra=()):
    path = work / (name + '.c')
    path.write_text(source, encoding='utf-8')
    exe = work / name
    cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-D_POSIX_C_SOURCE=200809L',
           '-Wall', '-Wextra', '-I', str(ROOT / 'src/core'), str(path)]
    cmd += [str(ROOT / x) for x in extra] + ['-pthread', '-o', str(exe)]
    subprocess.run(cmd, cwd=work, check=True)
    subprocess.run([str(exe)], cwd=work, check=True, timeout=25)

control = ((ROOT / 'src/mac/net_client.c').read_text(encoding='utf-8') + '\n' +
           (ROOT / 'src/mac/net_client_protocol.inc').read_text(encoding='utf-8') + '\n' +
           (ROOT / 'src/mac/net_client_transport.inc').read_text(encoding='utf-8'))
data = (ROOT / 'src/mac/xfer_receive.c').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='hopdesk-mac-tcp-') as directory:
    work = pathlib.Path(directory)
    fixture = (ROOT / 'tests/mac_listener_fixture.c.in').read_text(encoding='utf-8')
    fixture = fixture.replace('@CONTROL_ACCEPT@', extract(control, 'accept_control'))
    fixture = fixture.replace('@DATA_ACCEPT@', extract(data, 'accept_data'))
    assert 'if (!hop_session_matches(&session)) { close(s); continue; }' in extract(data, 'xfer_receive_connection_thread')
    run(work, 'mac_listeners', fixture)

    actual = re.sub(r'^#include[^\n]*\n', '', control, flags=re.M)
    actual = actual.replace('dispatch_async(dispatch_get_main_queue(),^{mac_capture_peer_geometry(pw,ph);});',
                            'mac_capture_peer_geometry(pw,ph);')
    actual = actual.replace('dispatch_async(dispatch_get_main_queue(),^{mac_capture_return();});',
                            'mac_capture_return();')
    assert 'dispatch_async' not in actual
    fixture = (ROOT / 'tests/mac_client_loopback_fixture.c.in').read_text(encoding='utf-8')
    fixture = fixture.replace('@PRODUCTION@', actual)
    run(work, 'mac_client_loopback', fixture,
        ['src/core/protocol.c', 'src/core/session.c', 'src/core/clock.c',
         'src/core/xfer_validate.c', 'src/core/crc32.c'])
