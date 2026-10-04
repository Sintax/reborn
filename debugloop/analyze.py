import bisect
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from . import config

HOOK_RE = re.compile(r"baseAddress\s*\+\s*(0x[0-9a-fA-F]+)\)\s*,\s*&?Hooks::(\w+)")
FUNCS_JSON = config.PKG / "ghidra" / "out" / "battleborn_functions.json"
GHIDRA_DIR = Path(os.environ["LOCALAPPDATA"]) / "Programs" / "Ghidra" / "ghidra_12.1.4_PUBLIC"


def known_addresses(src_dir: Path = config.REPO / "reborn") -> dict[int, str]:
    out = {}
    for p in src_dir.glob("*.cpp"):
        text = p.read_text(encoding="utf-8", errors="replace")
        text = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("//"))
        for m in HOOK_RE.finditer(text):
            out[int(m.group(1), 16)] = m.group(2)
    return out


def load_functions(path: Path = FUNCS_JSON) -> list[tuple[int, int, str]]:
    if not path.exists():
        return []
    try:
        data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
        return sorted((f["start"], f["end"], f["name"]) for f in data)
    except (ValueError, KeyError, TypeError, OSError):
        return []


def describe_frame(frame: str, funcs, hooks) -> str:
    mod, _, off = frame.partition("+")
    if mod not in ("battleborn", "serverborn"):
        return frame
    try:
        rva = int(off, 16)
    except ValueError:
        return frame
    parts = []
    i = bisect.bisect_right([f[0] for f in funcs], rva) - 1
    if i >= 0 and funcs[i][0] <= rva <= funcs[i][1]:
        parts.append(f"in {funcs[i][2]}")
        if funcs[i][0] in hooks:
            parts.append(f"hooked by {hooks[funcs[i][0]]}")
    elif rva in hooks:
        parts.append(f"hooked by {hooks[rva]}")
    return f"{frame} ({', '.join(parts)})" if parts else frame


def find_cdb() -> Path | None:
    if os.environ.get("BB_CDB") and Path(os.environ["BB_CDB"]).exists():
        return Path(os.environ["BB_CDB"])
    try:
        loc = subprocess.run(["powershell", "-NoProfile", "-Command",
                              "(Get-AppxPackage Microsoft.WinDbg).InstallLocation"],
                             capture_output=True, text=True, timeout=30).stdout.strip()
        if loc and (Path(loc) / "amd64" / "cdb.exe").exists():
            return Path(loc) / "amd64" / "cdb.exe"
    except (OSError, subprocess.TimeoutExpired):
        pass
    kit = Path(r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe")
    return kit if kit.exists() else None


def run_cdb(dump: Path, timeout: int = 240, cdb: Path | None = None) -> str:
    cdb = cdb or find_cdb()
    if not cdb:
        return "(cdb not found)"
    cache = Path(os.environ["LOCALAPPDATA"]) / "symcache"
    cmd = (f".symfix+ {cache}; .sympath+ \"{config.WIN64}\"; .reload; "
           "!analyze -v; .ecxr; kb 30; q")
    try:
        p = subprocess.run([str(cdb), "-z", str(dump), "-c", cmd], capture_output=True,
                           text=True, timeout=timeout, errors="replace")
        lines = p.stdout.splitlines()
        return "\n".join(lines[-300:])
    except OSError as e:
        return f"(cdb failed to start: {e})"
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        if out:
            return "\n".join(out.splitlines()[-300:]) + "\n(cdb timed out; partial output)"
        return "(cdb timed out)"


def write_triage(result) -> Path:
    d = result.run_dir
    funcs, hooks = load_functions(), known_addresses()
    cdb = find_cdb()
    o = result.outcome
    md = [f"# Triage: {result.signature}", "",
          f"- Scenario: `{result.scenario}`  run: `{result.run_id}`",
          f"- Outcome: **{o.kind}** {o.detail} (process `{o.process}`, phase `{o.phase}`, "
          f"after {result.elapsed_s} s)", ""]
    for rep in sorted(d.glob("*.crash.json")) + sorted(d.glob("*.hang.json")):
        try:
            r = json.loads(rep.read_text(encoding="utf-8", errors="replace"))
            head = [f"## {rep.name}", "", f"code `{r.get('code', 'hang')}` "
                    f"first_chance `{r.get('first_chance')}`", "", "Frames:", ""]
            head += [f"{i}. `{describe_frame(str(f), funcs, hooks)}`"
                     for i, f in enumerate(r.get("frames", []))]
            dump = d / r.get("dump", "")
        except (ValueError, KeyError, TypeError, AttributeError):
            md += [f"## {rep.name}", "", f"(unreadable report: {rep.name})", ""]
            continue
        if not dump.is_file():
            cdb_out = "(no dump)"
        elif cdb is None:
            cdb_out = "(cdb not found)"
        else:
            cdb_out = run_cdb(dump, cdb=cdb)
        md += head + ["", "cdb:", "", "```", cdb_out, "```", ""]
    for log in sorted(d.glob("*.log")):
        md += [f"## Last lines of {log.name}", "", "```",
               *log.read_text(encoding="utf-8", errors="replace").splitlines()[-60:], "```", ""]
    tl = d / "timeline.jsonl"
    if tl.exists():
        md += ["## Last samples", "", "```", *tl.read_text(encoding="utf-8", errors="replace").splitlines()[-12:], "```", ""]
    md += ["## Known hook addresses", "",
           *[f"- `battleborn+{hex(a)}` {n}" for a, n in sorted(hooks.items())]]
    out = d / "triage.md"
    out.write_text("\n".join(md), encoding="utf-8")
    return out


def export_ghidra_functions() -> Path:
    """One-time: Ghidra headless analysis of Battleborn.exe (can take an hour or more)."""
    proj = config.PKG / "ghidra" / "out" / "project"
    proj.mkdir(parents=True, exist_ok=True)
    cmd = [str(GHIDRA_DIR / "support" / "analyzeHeadless.bat"), str(proj), "bb",
           "-import", str(config.WIN64 / "Battleborn.exe"), "-overwrite",
           "-scriptPath", str(config.PKG / "ghidra"),
           "-postScript", "ExportFunctions.java", str(FUNCS_JSON), "-max-cpu", "4"]
    env = dict(os.environ, JAVA_HOME=r"C:\Program Files\Eclipse Adoptium\jdk-21.0.12.101-hotspot")
    subprocess.run(cmd, check=True, env=env)
    return FUNCS_JSON


if __name__ == "__main__":
    from .run import RunResult
    from .outcome import Outcome
    d = Path(sys.argv[1])
    res = json.loads((d / "result.json").read_text(encoding="utf-8", errors="replace"))
    r = RunResult(res["run_id"], res["scenario"], Outcome(**res["outcome"]), res["signature"],
                  d, res["elapsed_s"])
    print(write_triage(r))
