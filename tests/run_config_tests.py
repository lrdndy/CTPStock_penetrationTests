#!/usr/bin/env python3
"""Compile/run offline tests using C++17, SDK headers, and no SDK libraries.

Linux: python3 tests/run_config_tests.py
Windows (x64 Native Tools prompt): py tests/run_config_tests.py
Set CXX to select g++, clang++, or cl. All generated files use a temporary directory.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def prompt_test(binary):
    if os.name == "nt":
        print("SKIP interactive Windows console test (verify hidden fallback manually).")
        return
    import pty
    import select
    import termios
    import time

    master, slave = pty.openpty()
    original = termios.tcgetattr(slave)
    process = subprocess.Popen([str(binary), "--prompt"], stdin=slave,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 5
        prefix = b""
        while b"OFFLINE_PROMPT: " not in prefix and time.monotonic() < deadline:
            if select.select([process.stdout], [], [], 0.1)[0]:
                chunk = os.read(process.stdout.fileno(), 1024)
                if not chunk:
                    break
                prefix += chunk
        assert b"OFFLINE_PROMPT: " in prefix, "Hidden prompt did not start"
        while termios.tcgetattr(slave)[3] & termios.ECHO and time.monotonic() < deadline:
            time.sleep(0.01)
        assert not termios.tcgetattr(slave)[3] & termios.ECHO, "Terminal echo was not disabled"
        os.write(master, b"DummyPrompt#;=42\n")
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 0, "Hidden prompt test failed"
        assert b"DummyPrompt#;=42" not in prefix + stdout + stderr, "Prompt output exposed test input"
        assert termios.tcgetattr(slave)[3] & termios.ECHO == original[3] & termios.ECHO, "Terminal echo not restored"
        print("PASS POSIX hidden prompt accepts input, disables echo, and restores echo")
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
        os.close(master)
        os.close(slave)


def main():
    root = Path(__file__).resolve().parents[1]
    compiler = os.environ.get("CXX") or ("cl" if os.name == "nt" else "g++")
    if not shutil.which(compiler):
        raise SystemExit("C++ compiler unavailable; set CXX or use an x64 Native Tools prompt.")
    with tempfile.TemporaryDirectory(prefix="ctpstock-config-tests-") as temporary:
        build = Path(temporary)
        binary = build / ("config_tests.exe" if os.name == "nt" else "config_tests")
        source = root / "tests" / "config_credentials_test.cpp"
        includes = root / "sdk" / "include"
        if Path(compiler).name.lower() in {"cl", "cl.exe"}:
            command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/utf-8", "/Gy", "/Gw", "/O2",
                       f"/I{includes}", str(source), f"/Fe:{binary}", "/link", "/OPT:REF"]
        else:
            command = [compiler, "-std=c++17", "-O1", "-pthread", "-ffunction-sections", "-fdata-sections",
                       "-I", str(includes), str(source), "-Wl,--gc-sections", "-o", str(binary)]
        subprocess.run(command, cwd=build, check=True)
        subprocess.run([str(binary), str(build / "fixtures")], stdin=subprocess.DEVNULL, check=True)
        prompt_test(binary)
    print("Offline tests complete. Live login and Windows console behavior are separate checks.")


if __name__ == "__main__":
    try:
        main()
    except (subprocess.SubprocessError, AssertionError) as error:
        print(f"FAIL offline test runner: {type(error).__name__}", file=sys.stderr)
        sys.exit(1)
