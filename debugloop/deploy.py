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


class DeployError(RuntimeError):
    pass


def _backup(p: Path, win64: Path) -> Path:
    """Copy p into win64/rb_backups under a name that is never reused."""
    d = win64 / "rb_backups"
    d.mkdir(exist_ok=True)
    stem = f"{p.name}.{datetime.now():%Y%m%d-%H%M%S-%f}"
    n = 0
    while True:
        b = d / (stem if n == 0 else f"{stem}.{n}")
        try:
            # Exclusive create: never overwrite an existing backup.
            with open(b, "xb") as out, open(p, "rb") as src:
                shutil.copyfileobj(src, out)
            break
        except FileExistsError:
            n += 1
    shutil.copystat(p, b)
    return b


def _replace(src: Path, dst: Path, win64: Path, backups: list[Path] | None = None) -> None:
    """Back up dst if it exists, then copy src over it."""
    if dst.exists():
        b = _backup(dst, win64)
        if backups is not None:
            backups.append(b)
    shutil.copy2(src, dst)


def _require_game_exe(win64: Path) -> Path:
    exe = win64 / "Battleborn.exe"
    if not exe.exists():
        raise DeployError(f"Battleborn.exe not found in {win64}; "
                          "set BB_GAME_DIR to the Battleborn install folder")
    return exe


def ensure_serverborn(win64: Path = config.WIN64) -> Path:
    exe, server = _require_game_exe(win64), win64 / "Serverborn.exe"
    if _sha(exe) != _sha(server):
        _replace(exe, server, win64)
    return server


def deploy(dll: Path = config.MOD_DLL, win64: Path = config.WIN64) -> DeployResult:
    _require_game_exe(win64)   # fail before touching anything if this is not the game folder
    r = DeployResult()
    target = win64 / "reborn.dll"
    if _sha(dll) != _sha(target):
        _replace(dll, target, win64, r.backups)
        r.changed.append("reborn.dll")
    pdb = dll.with_suffix(".pdb")
    if pdb.exists() and _sha(pdb) != _sha(win64 / "reborn.pdb"):
        _replace(pdb, win64 / "reborn.pdb", win64)
    ensure_serverborn(win64)
    return r
