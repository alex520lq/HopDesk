#!/usr/bin/env python3
"""Extract release notes and a single-line manifest summary from CHANGELOG.md.

Usage:
  python3 scripts/release_notes.py <tag> <changelog_path> <notes_out> <summary_out>
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent


def read_protocol_versions(hop_h: Path):
    text = hop_h.read_text(encoding='utf-8')
    ctrl = re.search(r'#define\s+HOP_CTRL_PROTO_VER\s+(\d+)', text)
    xfer = re.search(r'#define\s+HOP_XFER_PROTO_VER\s+(\d+)', text)
    if not ctrl or not xfer:
        raise ValueError('Could not read HOP_CTRL_PROTO_VER / HOP_XFER_PROTO_VER from src/core/hop.h')
    return int(ctrl.group(1)), int(xfer.group(1))


def extract_section(changelog_text: str, tag: str) -> str:
    pattern = re.compile(
        r'^##\s+\[' + re.escape(tag) + r'\][^\n]*\n(.*?)(?=^##\s+\[|\Z)',
        re.MULTILINE | re.DOTALL,
    )
    match = pattern.search(changelog_text)
    if not match:
        return ''
    return match.group(1).strip()


def first_summary_line(section: str, tag: str, channel: str) -> str:
    for raw in section.splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith(('- ', '* ')):
            line = line[2:].strip()
        # Strip simple markdown emphasis/code ticks for clean single-line UI display.
        line = re.sub(r'[`*]', '', line)
        line = re.sub(r'\s+', ' ', line).strip()
        if line:
            # Keep UTF-8 byte length well inside HOP_UPDATE_SUMMARY_MAX (160 bytes).
            encoded = line.encode('utf-8')
            if len(encoded) > 140:
                while len(line.encode('utf-8')) > 136:
                    line = line[:-1]
                line = line.rstrip() + '…'
            return line
    label = '开发版' if channel == 'dev' else '稳定版'
    return f'HopDesk {tag}（{label}）发布'


def build_release_notes(tag: str, changelog_path: Path, hop_h: Path):
    if not re.fullmatch(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-dev\.[1-9][0-9]*)?', tag):
        raise ValueError(f'Invalid release tag: {tag}')
    version = tag[1:]
    channel = 'dev' if '-dev.' in version else 'stable'
    channel_zh = '开发版 (dev)' if channel == 'dev' else '稳定版 (stable)'
    ctrl_proto, xfer_proto = read_protocol_versions(hop_h)
    changelog_text = changelog_path.read_text(encoding='utf-8') if changelog_path.exists() else ''
    section = extract_section(changelog_text, tag)
    summary = first_summary_line(section, tag, channel)

    header = (
        f'## HopDesk {tag}\n\n'
        f'- **版本**：`v{version}`\n'
        f'- **发布通道**：{channel_zh}\n'
        f'- **协议版本**：控制通道 `v{ctrl_proto}` / 数据通道 `v{xfer_proto}`\n\n'
    )
    body = section if section else '本次发布未在 `CHANGELOG.md` 中填写独立条目，请查看提交历史。'
    notes = header + body + '\n'
    return {
        'version': version,
        'channel': channel,
        'ctrl_proto': ctrl_proto,
        'xfer_proto': xfer_proto,
        'notes': notes,
        'summary': summary,
    }


def main():
    if len(sys.argv) != 5:
        raise SystemExit('Usage: release_notes.py <tag> <changelog> <notes_out> <summary_out>')
    tag = sys.argv[1]
    changelog_path = Path(sys.argv[2])
    notes_out = Path(sys.argv[3])
    summary_out = Path(sys.argv[4])
    result = build_release_notes(tag, changelog_path, ROOT / 'src/core/hop.h')
    notes_out.parent.mkdir(parents=True, exist_ok=True)
    summary_out.parent.mkdir(parents=True, exist_ok=True)
    notes_out.write_text(result['notes'], encoding='utf-8')
    summary_out.write_text(result['summary'], encoding='utf-8')
    print(f"channel={result['channel']} ctrl_proto={result['ctrl_proto']} xfer_proto={result['xfer_proto']}")


if __name__ == '__main__':
    main()
