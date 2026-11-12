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
