import subprocess
from pathlib import Path

import pytest

from debugloop import build, config

SRC = config.REPO / "reborn"
OUT = config.NATIVE_OUT


def build_and_run(name: str, sources: list[Path], args=(), timeout=60):
    res = build.build_native(name, sources, OUT, [f'/I"{SRC}"'])
    assert res.ok, res.output[-4000:]
    return subprocess.run([str(res.artifact), *args], capture_output=True, text=True,
                          timeout=timeout)


def test_launch_options():
    p = build_and_run("test_launch_options",
                      [SRC / "tests" / "test_launch_options.cpp", SRC / "LaunchOptions.cpp"])
    assert p.returncode == 0, p.stdout + p.stderr
    assert "PASS" in p.stdout
