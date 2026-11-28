#!/usr/bin/env python3
"""Real core gate + receipt + publisher policy tests. On POSIX also execute the
production macOS helper with OS command doubles and real temporary file moves.
Not a substitute for Windows/AppKit desktop acceptance.
"""
import importlib.util
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import zipfile
ROOT = Path(__file__).resolve().parent.parent

def load_policy():
    spec = importlib.util.spec_from_file_location('policy',ROOT/'scripts/update_manifest_policy.py')
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m

def manifest(version,hashchar='a'):
    return ('format=1\nversion='+version+'\ntag=v'+version+'\n'+
        'windows_url=https://github.com/alex520lq/HopDesk-Releases/releases/download/v'+version+'/HopDesk-Windows.zip\n'+
        'windows_sha256='+hashchar*64+'\n'+
        'macos_url=https://github.com/alex520lq/HopDesk-Releases/releases/download/v'+version+'/HopDesk-macOS.zip\n'+
        'macos_sha256='+'b'*64+'\n'+
        'notes_url=https://github.com/alex520lq/HopDesk-Releases/releases/tag/v'+version+'\n')

def manifest_v2(version,channel,ctrl=8,xfer=8,summary='Test release',hashchar='a'):
    return ('format=2\nchannel='+channel+'\nversion='+version+'\ntag=v'+version+'\n'+
        f'ctrl_proto={ctrl}\nxfer_proto={xfer}\n'+
        'windows_url=https://github.com/alex520lq/HopDesk-Releases/releases/download/v'+version+'/HopDesk-Windows.zip\n'+
        'windows_sha256='+hashchar*64+'\n'+
        'macos_url=https://github.com/alex520lq/HopDesk-Releases/releases/download/v'+version+'/HopDesk-macOS.zip\n'+
        'macos_sha256='+'b'*64+'\n'+
        'notes_url=https://github.com/alex520lq/HopDesk-Releases/releases/tag/v'+version+'\n'+
        'summary='+summary+'\n')

with tempfile.TemporaryDirectory(prefix='hopdesk-updates-') as tmp:
    work = Path(tmp)
    src = [ROOT/'tests/update_state_tests.c',ROOT/'src/core/update_state.c']
    exe = work/('state.exe' if os.name=='nt' else 'state')
    if os.name=='nt' and shutil.which('cl'):
        cmd=['cl','/nologo','/std:c11','/W4','/utf-8','/D_CRT_SECURE_NO_WARNINGS','/I'+str(ROOT/'src/core'),'/Fe:'+str(exe)]+list(map(str,src))
    else:
        cc=os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
        cmd=[cc,'-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-pthread','-I',str(ROOT/'src/core')]+list(map(str,src))+['-o',str(exe)]
    subprocess.run(cmd,cwd=work,check=True)
    subprocess.run([str(exe)],check=True,timeout=20)
    ready=work/'ready'
    env=os.environ|{'HOPDESK_UPDATE_READY':str(ready),'HOPDESK_UPDATE_NONCE':'a'*32,'HOPDESK_UPDATE_VERSION':'0.5.1'}
    subprocess.run([str(exe),'--ready','0.5.0'],env=env,check=True)
    assert not ready.exists(), 'wrong binary version acknowledged update'
    env['HOPDESK_UPDATE_NONCE']='bad'
    subprocess.run([str(exe),'--ready','0.5.1'],env=env,check=True)
    assert not ready.exists(), 'malformed transaction acknowledged'
    env['HOPDESK_UPDATE_NONCE']='a'*32
    p=subprocess.Popen([str(exe),'--ready','0.5.1'],env=env)
    p.wait(timeout=10)
    assert p.returncode==0 and ready.read_text().splitlines()==['a'*32,'0.5.1',str(p.pid)]
    print('PASS: readiness receipt binds version, nonce and process ID')
    policy=load_policy()
    candidate,current=work/'candidate',work/'latest.txt'
    candidate.write_text(manifest('0.5.2'),encoding='utf-8')
    assert policy.promote(candidate,current)=='promoted'
    candidate.write_text(manifest('0.5.1'),encoding='utf-8')
    assert policy.promote(candidate,current)=='skip-older'
    assert 'version=0.5.2' in current.read_text()
    candidate.write_text(manifest('0.5.2'),encoding='utf-8')
    assert policy.promote(candidate,current)=='unchanged'
    before=current.read_bytes()
    for bad in [manifest('0.5.2','c'),manifest('0.05.3'),manifest('0.5.3')+'unknown=x\n']:
        candidate.write_text(bad,encoding='utf-8')
        try:policy.promote(candidate,current)
        except ValueError:pass
        else:raise AssertionError('invalid/same-version mutation was accepted')
        assert current.read_bytes()==before
    candidate.write_text(manifest('0.5.3'),encoding='utf-8')
    assert policy.promote(candidate,current)=='promoted'

    # Dual-channel format=2 monotonic policy & isolation checks
    stable_ptr, dev_ptr = work/'stable.txt', work/'dev.txt'
    candidate.write_text(manifest_v2('0.1.0', 'stable'), encoding='utf-8')
    assert policy.promote(candidate, stable_ptr) == 'promoted'
    candidate.write_text(manifest_v2('0.2.0-dev.1', 'dev'), encoding='utf-8')
    assert policy.promote(candidate, dev_ptr) == 'promoted'
    candidate.write_text(manifest_v2('0.2.0-dev.2', 'dev'), encoding='utf-8')
    assert policy.promote(candidate, dev_ptr) == 'promoted'
    candidate.write_text(manifest_v2('0.2.0-dev.1', 'dev'), encoding='utf-8')
    assert policy.promote(candidate, dev_ptr) == 'skip-older'
    # Cross-channel overwrite and version/channel mismatch must be rejected
    for bad_v2, target_ptr in [
        (manifest_v2('0.2.0-dev.3', 'dev'), stable_ptr),
        (manifest_v2('0.2.0', 'stable'), dev_ptr),
        (manifest_v2('0.2.0', 'dev'), dev_ptr),
        (manifest_v2('0.2.0-dev.3', 'stable'), stable_ptr),
    ]:
        candidate.write_text(bad_v2, encoding='utf-8')
        try: policy.promote(candidate, target_ptr)
        except ValueError: pass
        else: raise AssertionError('cross-channel pollution was accepted')

    # Verify release notes extractor against repository CHANGELOG.md
    notes_out, summary_out = work/'release-notes.md', work/'summary.txt'
    subprocess.run([sys.executable, str(ROOT/'scripts/release_notes.py'),
                    'v0.1.0', str(ROOT/'CHANGELOG.md'), str(notes_out), str(summary_out)], check=True)
    notes_txt = notes_out.read_text(encoding='utf-8')
    assert 'v0.1.0' in notes_txt and '稳定版 (stable)' in notes_txt and '控制通道' in notes_txt
    assert summary_out.read_text(encoding='utf-8').strip() != ''
    print('PASS: publisher rejects rollback, same-version mutation, cross-channel pollution, and extracts release notes')

    # Source-level contract checks supplement (not replace) native helper tests.
    assert not (ROOT/'scripts/update-windows.ps1').read_bytes().startswith(b'\xef\xbb\xbf')
    win=(ROOT/'src/win/update.c').read_text(encoding='utf-8')
    assert 'has_bom' in win
    assert win.index('hop_update_install_begin(xfer_busy)')<win.index('CreateProcessW(')
    after_spawn=win[win.index('    CloseHandle(pi.hThread);'):win.index('    return 1; /* admission')]
    assert 'prepared_ok' in after_spawn and 'PostMessageW(owner, WM_CLOSE' in after_spawn
    assert 'MB_OK | MB_ICONINFORMATION' not in after_spawn
    for platform in ('win','mac'):
        transport=(ROOT/f'src/{platform}/xfer.c').read_text(encoding='utf-8')
        sender=(ROOT/f'src/{platform}/xfer_submit.c').read_text(encoding='utf-8')
        for name in ('xfer_submit_paths','xfer_submit_image'):
            wrapper=sender[sender.rindex('int '+name+'('):]
            assert wrapper.index('hop_update_work_enter')<wrapper.index(name+'_impl(')<wrapper.index('hop_update_work_leave')
        start=transport.index('static void xfer_data_frame(void')
        body=transport[start:transport.index('\n}',start)]
        assert body.index('hop_update_receive_enter')<body.index('xfer_receive_process_frame(')<body.index('hop_update_work_leave')
    workflow=(ROOT/'.github/workflows/build.yml').read_text(encoding='utf-8')
    assert workflow.index('Guard existing version assets before publication') < workflow.index('name: Publish GitHub Release assets')
    assert 'update_manifest_policy.py "release-assets/$CHANNEL_FILE" "release-repo/$CHANNEL_FILE"' in workflow
    assert 'update_manifest_policy.py release-assets/latest.txt release-repo/latest.txt' in workflow
    print('PASS: install ordering and both-platform producer/receiver admission contracts')

    if os.name!='nt':
        for mode in ('healthy','crash','silent','wrong_nonce','early_crash','replace_fail','old_alive'):
            case=work/mode;case.mkdir();bins=case/'bin';bins.mkdir();transaction=case/'transaction';transaction.mkdir()
            target=case/'HopDesk.app';target.mkdir();(target/'version').write_text('old')
            archive=case/'update.zip'
            # New executable provides a real child PID and a transaction receipt.
            app='#!/bin/bash\n'
            if mode=='crash':app+='exit 7\n'
            elif mode=='silent':app+='while true; do /bin/sleep 1; done\n'
            else:
                nonce='wrong' if mode=='wrong_nonce' else '$HOPDESK_UPDATE_NONCE'
                app+=f'printf "%s\\n%s\\n%s\\n" "{nonce}" "$HOPDESK_UPDATE_VERSION" "$$" > "$HOPDESK_UPDATE_READY.tmp"\nmv "$HOPDESK_UPDATE_READY.tmp" "$HOPDESK_UPDATE_READY"\nwhile true; do /bin/sleep 1; done\n'
            if mode=='early_crash':app=app.replace('while true; do /bin/sleep 1; done','/bin/sleep 0.02; exit 9')
            with zipfile.ZipFile(archive,'w') as z:
                z.writestr('HopDesk.app/Contents/MacOS/HopDesk',app)
                z.writestr('HopDesk.app/version','new')
            wrappers={
                'ditto':f'''#!{sys.executable}
import sys,zipfile,shutil,os
if sys.argv[1:3]==['-x','-k']:
 zipfile.ZipFile(sys.argv[3]).extractall(sys.argv[4]);os.chmod(sys.argv[4]+'/HopDesk.app/Contents/MacOS/HopDesk',0o755)
else:shutil.copytree(sys.argv[1],sys.argv[2])
''',
                'PlistBuddy':'#!/bin/sh\ncase "$2" in *Identifier) echo com.hopdesk.app;; *) echo 0.5.1;; esac\n',
                'codesign':'#!/bin/sh\nexit 0\n',
                'open':'#!/bin/sh\ntouch "'+str(case/'old-restarted')+'"\n',
                'sleep':'#!/bin/sh\n/bin/sleep 0.005\n',
                'mv':'#!/bin/bash\n'+('case "$1" in *.update.*) exit 1;; esac\n' if mode=='replace_fail' else '')+'/bin/mv "$@"\n',
            }
            for name,body in wrappers.items():p=bins/name;p.write_text(body,encoding='utf-8');p.chmod(0o755)
            helper=case/'helper.sh'
            helper.write_text((ROOT/'scripts/update-macos.command').read_text(encoding='utf-8').replace('/usr/libexec/PlistBuddy',str(bins/'PlistBuddy')),encoding='utf-8')
            old=subprocess.Popen(['/bin/sleep','30']) if mode=='old_alive' else None
            pid=str(old.pid) if old else '2147483647'
            env=os.environ|{'PATH':str(bins)+':'+os.environ['PATH'],'TMPDIR':str(case)+'/'}
            result=subprocess.run(['bash',str(helper),str(archive),str(target),pid,str(transaction),'a'*32,'0.5.1'],env=env,capture_output=True,text=True,timeout=15)
            if old:old.terminate();old.wait()
            if mode=='healthy':
                assert result.returncode==0 and (target/'version').read_text()=='new',result.stderr
            else:
                assert result.returncode!=0 and (target/'version').read_text()=='old',(mode,result.stderr)
                assert (case/'old-restarted').exists()==(mode!='old_alive')
            assert not list(case.glob('HopDesk.app.previous.*'))
            assert not list(case.glob('HopDesk.app.update.*'))
            assert not transaction.exists()
            # A successful child survives installation; stop this test child only.
            if mode=='healthy':
                # Its executable path is unique to this disposable fixture.
                subprocess.run(['pkill','-f','^/bin/bash '+str(target/'Contents/MacOS/HopDesk')+'$'],check=False)
            print('PASS: production macOS updater '+mode+' (OS doubles, real filesystem/processes)')

        # Verify dev prerelease version and local certificate signature preservation
        case=work/'dev_resign';case.mkdir();bins=case/'bin';bins.mkdir();transaction=case/'transaction';transaction.mkdir()
        target=case/'HopDesk.app';target.mkdir();(target/'version').write_text('old')
        archive=case/'update.zip'
        dev_ver='0.2.0-dev.1'
        cert_hash='8816FF22E00A692C99E464FC78068E98A2D5B9BB'
        app='#!/bin/bash\nprintf "%s\\n%s\\n%s\\n" "$HOPDESK_UPDATE_NONCE" "$HOPDESK_UPDATE_VERSION" "$$" > "$HOPDESK_UPDATE_READY.tmp"\nmv "$HOPDESK_UPDATE_READY.tmp" "$HOPDESK_UPDATE_READY"\nwhile true; do /bin/sleep 1; done\n'
        with zipfile.ZipFile(archive,'w') as z:
            z.writestr('HopDesk.app/Contents/MacOS/HopDesk',app)
            z.writestr('HopDesk.app/version','new-dev')
        resign_log=case/'codesign.log'
        wrappers={
            'ditto':f'''#!{sys.executable}
import sys,zipfile,shutil,os
if sys.argv[1:3]==['-x','-k']:
 zipfile.ZipFile(sys.argv[3]).extractall(sys.argv[4]);os.chmod(sys.argv[4]+'/HopDesk.app/Contents/MacOS/HopDesk',0o755)
else:shutil.copytree(sys.argv[1],sys.argv[2])
''',
            'PlistBuddy':f'#!/bin/sh\ncase "$2" in *Identifier) echo com.hopdesk.app;; *) echo {dev_ver};; esac\n',
            'codesign':f'''#!/bin/sh
echo "$@" >> "{resign_log}"
if [ "$1" = "-d" ]; then
  echo 'designated => identifier "com.hopdesk.app" and certificate leaf = H"{cert_hash}"'
fi
exit 0
''',
            'open':'#!/bin/sh\ntouch "'+str(case/'old-restarted')+'"\n',
            'sleep':'#!/bin/sh\n/bin/sleep 0.005\n',
        }
        for name,body in wrappers.items():p=bins/name;p.write_text(body,encoding='utf-8');p.chmod(0o755)
        helper=case/'helper.sh'
        helper.write_text((ROOT/'scripts/update-macos.command').read_text(encoding='utf-8').replace('/usr/libexec/PlistBuddy',str(bins/'PlistBuddy')),encoding='utf-8')
        env=os.environ|{'PATH':str(bins)+':'+os.environ['PATH'],'TMPDIR':str(case)+'/'}
        result=subprocess.run(['bash',str(helper),str(archive),str(target),'2147483647',str(transaction),'b'*32,dev_ver],env=env,capture_output=True,text=True,timeout=15)
        assert result.returncode==0 and (target/'version').read_text()=='new-dev',result.stderr
        assert f'--force --deep --sign {cert_hash}' in resign_log.read_text(encoding='utf-8'),resign_log.read_text(encoding='utf-8')
        subprocess.run(['pkill','-f','^/bin/bash '+str(target/'Contents/MacOS/HopDesk')+'$'],check=False)
        print('PASS: production macOS updater dev-version & stable signing certificate preservation')
print('All update lifecycle regressions passed.')
