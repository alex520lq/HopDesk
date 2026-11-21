#!/usr/bin/env python3
"""Run the production macOS transfer modules against a loopback protocol peer."""

import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix="hopdesk-mac-receive-") as directory:
    work = pathlib.Path(directory)
    exe = work / "mac-receive-tests"
    sources = [ROOT / "tests/mac_receive_transfer_tests.c"]
    sources += sorted((ROOT / "src/core").glob("*.c"))
    sources += sorted((ROOT / "src/mac").glob("xfer*.c"))
    subprocess.run(
        [os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-I", str(ROOT / "src/core"), "-pthread", "-o", str(exe), *map(str, sources)],
        check=True,
    )
    subprocess.run(
        [str(exe)], check=True, timeout=25,
        env={**os.environ, "HOME": directory, "TMPDIR": directory},
    )

    paste = (ROOT / "src/mac/clipboard_paste.inc").read_text(encoding="utf-8")
    focus = paste[paste.index("static id clip_ax_value"):paste.index("static NSString *clip_finder_destination")]
    fixture = (ROOT / "tests/mac_finder_focus_fixture.m.in").read_text(encoding="utf-8")
    source = work / "finder-focus.m"
    source.write_text(fixture.replace("@FINDER_RESOLUTION@", focus), encoding="utf-8")
    exe = work / "finder-focus"
    subprocess.run(
        [os.environ.get("CC", "clang"), "-Wall", "-Wextra", "-Werror", "-fobjc-arc",
         "-framework", "AppKit", "-framework", "ApplicationServices", "-o", str(exe), str(source)],
        check=True,
    )
    subprocess.run([str(exe)], check=True, timeout=10)
