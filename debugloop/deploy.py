import hashlib
import shutil
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path

from . import config


@dataclass
class DeployResult:
    changed: list[str] = field(default_factory=list)
    backups: list[Path] = field(default_factory=list)


def _sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else ""


def _backup(p: Path, win64: Path) -> Path:
    d = win64 / "rb_backups"
    d.mkdir(exist_ok=True)
    b = d / f"{p.name}.{datetime.now():%Y%m%d-%H%M%S}"
    shutil.copy2(p, b)
    return b


def ensure_serverborn(win64: Path = config.WIN64) -> Path:
    exe, server = win64 / "Battleborn.exe", win64 / "Serverborn.exe"
    if _sha(exe) != _sha(server):
        shutil.copy2(exe, server)
    return server


def deploy(dll: Path = config.MOD_DLL, win64: Path = config.WIN64) -> DeployResult:
    r = DeployResult()
    target = win64 / "reborn.dll"
    if _sha(dll) != _sha(target):
        if target.exists():
            r.backups.append(_backup(target, win64))
        shutil.copy2(dll, target)
        r.changed.append("reborn.dll")
    pdb = dll.with_suffix(".pdb")
    if pdb.exists() and _sha(pdb) != _sha(win64 / "reborn.pdb"):
        shutil.copy2(pdb, win64 / "reborn.pdb")
    ensure_serverborn(win64)
    return r
