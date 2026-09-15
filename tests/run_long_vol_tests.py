#!/usr/bin/env python3
"""Offline long-volatility strategy checks; fake SDK, no network/orders."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CXX") or ("cl" if os.name == "nt" else "g++")
if not shutil.which(compiler):
    raise SystemExit("C++17 compiler missing; use an x64 Native Tools prompt on Windows.")
with tempfile.TemporaryDirectory(prefix="ctp-long-vol-tests-") as directory:
    binary = Path(directory) / ("long_vol_tests.exe" if os.name == "nt" else "long_vol_tests")
    source = root / "tests" / "long_volatility_test.cpp"
    includes = root / "sdk" / "include"
    if Path(compiler).name.lower() in {"cl", "cl.exe"}:
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/Gy", "/Gw", "/O2",
                   f"/I{includes}", str(source), f"/Fe:{binary}", "/link", "/OPT:REF", "advapi32.lib", "user32.lib"]
    else:
        command = [compiler, "-std=c++17", "-O1", "-pthread", "-ffunction-sections", "-fdata-sections",
                   "-isystem", str(includes), str(source), "-Wl,--gc-sections", "-o", str(binary)]
    subprocess.run(command, cwd=directory, check=True)
    subprocess.run([str(binary), str(Path(directory) / "fixtures")], stdin=subprocess.DEVNULL, check=True)
