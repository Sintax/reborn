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


import json
import time


def _crashme():
    res = build.build_native("crashme", [SRC / "tests" / "crashme.cpp", SRC / "Diagnostics.cpp"],
                             OUT, [f'/I"{SRC}"'])
    assert res.ok, res.output[-4000:]
    return res.artifact


def test_crash_writes_dump_and_report(tmp_path):
    p = subprocess.run([str(_crashme()), "crash", str(tmp_path)], capture_output=True, text=True,
                       timeout=60)
    assert p.returncode != 0
    reports = sorted(tmp_path.glob("crashme.*.crash.json"))
    assert reports, list(tmp_path.iterdir())
    r = json.loads(reports[0].read_text())
    assert r["code"] == "0xC0000005"
    assert r["frames"][0].startswith("crashme+0x")
    assert (tmp_path / r["dump"]).stat().st_size > 10_000
    assert any("hello from crashme" in line for line in r["log_tail"])
    assert "hello from crashme" in (tmp_path / "crashme.log").read_text()


def test_hang_writes_hang_report(tmp_path):
    proc = subprocess.Popen([str(_crashme()), "hang", str(tmp_path)])
    try:
        deadline = time.time() + 30
        while time.time() < deadline and not (tmp_path / "crashme.hang.json").exists():
            time.sleep(0.5)
        r = json.loads((tmp_path / "crashme.hang.json").read_text())
        assert r["seconds"] == 5
        assert any(f.startswith(("ntdll+", "kernelbase+")) for f in r["frames"])
        assert (tmp_path / "crashme.hang.dmp").exists()
    finally:
        proc.kill()


def test_spam_is_deduplicated_and_capped(tmp_path):
    p = subprocess.run([str(_crashme()), "spam", str(tmp_path)], capture_output=True, text=True,
                       timeout=60)
    assert p.returncode == 0, p.stdout + p.stderr
    reports = list(tmp_path.glob("crashme.*.crash.json"))
    assert len(reports) == 5
    assert all(json.loads(r.read_text())["first_chance"] for r in reports)
