import configparser
import ctypes
import hashlib
import shutil
import subprocess
import time
import zlib
from pathlib import Path
from typing import Protocol

import psutil

from . import config

GAME_BASE_ARGS = ["-windowed", "-ResX=960", "-ResY=540", "-nosound", "-nomoviestartup", "-NOSPLASH"]
GAME_EXES = ("Battleborn.exe", "Serverborn.exe")
STEAMID_BASE = 76561197960287930
LOADER_FILES = ("steamclient_loader_x64.exe", "steamclient64.dll", "steamclient.dll")

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.OpenProcess.restype = ctypes.c_void_p
PROCESS_TERMINATE, PROCESS_QUERY_LIMITED, SYNCHRONIZE = 0x0001, 0x1000, 0x100000
STILL_ACTIVE = 259


class ProcessHandle:
    """Works for processes we did not spawn directly (the loader spawns the game)."""

    def __init__(self, pid: int):
        self.pid = pid
        self._final: int | None = None
        self._h = _k32.OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED | SYNCHRONIZE, False, pid)
        if not self._h:
            raise OSError(f"cannot open process {pid}")

    def close(self) -> None:
        """Release the Win32 handle; the last exit code stays readable."""
        if self._h:
            self._final = self.exit_code()
            _k32.CloseHandle(ctypes.c_void_p(self._h))
            self._h = None

    def exit_code(self) -> int | None:
        if not self._h:
            return self._final
        code = ctypes.c_ulong()
        if not _k32.GetExitCodeProcess(ctypes.c_void_p(self._h), ctypes.byref(code)):
            return None
        return None if code.value == STILL_ACTIVE else ctypes.c_int32(code.value).value

    def kill(self) -> None:
        if self._h and self.exit_code() is None:
            _k32.TerminateProcess(ctypes.c_void_p(self._h), 1)
            _k32.WaitForSingleObject(ctypes.c_void_p(self._h), 10000)


class Launcher(Protocol):
    def start(self, name: str, role: str, args: list[str]) -> ProcessHandle: ...


def _sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else ""


def ensure_identity(name: str, exe_name: str, args: list[str], win64: Path = config.WIN64) -> Path:
    loader = win64 / LOADER_FILES[0]
    blocked_hint = (f"{loader} is {{}}; antivirus (Windows Defender) often quarantines this "
                    "Steam emulator loader - restore or allow it in Windows Security, then retry")
    if not loader.exists():
        raise RuntimeError(blocked_hint.format("missing"))
    d = win64 / "rb_ids" / name
    (d / "steam_settings").mkdir(parents=True, exist_ok=True)
    for f in LOADER_FILES:
        try:
            changed = (win64 / f).exists() and _sha(win64 / f) != _sha(d / f)
        except OSError as e:
            if f == LOADER_FILES[0]:
                raise RuntimeError(blocked_hint.format(f"unreadable ({e.strerror})")) from e
            raise
        if changed:
            shutil.copy2(win64 / f, d / f)
    if (win64 / "steam_settings").is_dir():
        shutil.copytree(win64 / "steam_settings", d / "steam_settings", dirs_exist_ok=True)

    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    if (win64 / "ColdClientLoader.ini").exists():
        ini.read(win64 / "ColdClientLoader.ini", encoding="utf-8")
    if "SteamClient" not in ini:
        ini["SteamClient"] = {}
    sc = ini["SteamClient"]
    sc["Exe"] = str(win64 / exe_name)
    sc["ExeRunDir"] = str(win64)
    sc["ExeCommandLine"] = subprocess.list2cmdline(args)
    sc.setdefault("AppId", "394230")
    sc["SteamClientDll"] = str(d / "steamclient.dll")
    sc["SteamClient64Dll"] = str(d / "steamclient64.dll")
    with open(d / "ColdClientLoader.ini", "w", encoding="utf-8") as f:
        ini.write(f)

    user = configparser.ConfigParser(interpolation=None)
    user.optionxform = str
    user["user::general"] = {"account_name": name,
                             "account_steamid": str(STEAMID_BASE + zlib.crc32(name.encode()) % 100000),
                             "language": "english"}
    user["user::saves"] = {"local_save_path": str(d / "saves")}
    with open(d / "steam_settings" / "configs.user.ini", "w", encoding="utf-8") as f:
        user.write(f)
    return d


def find_game_processes() -> list[int]:
    return [p.pid for p in psutil.process_iter(["name"]) if p.info["name"] in GAME_EXES]


class RealLauncher:
    def __init__(self, win64: Path = config.WIN64):
        self.win64 = win64

    def start(self, name: str, role: str, args: list[str]) -> ProcessHandle:
        exe = "Serverborn.exe" if role == "server" else "Battleborn.exe"
        d = ensure_identity(name, exe, args, self.win64)
        loader = subprocess.Popen([str(d / "steamclient_loader_x64.exe")], cwd=d)
        marker = f"-rbinstance={name}"
        deadline = time.time() + 60
        while time.time() < deadline:
            for p in psutil.process_iter(["name", "cmdline"]):
                if p.info["name"] == exe and marker in (p.info["cmdline"] or []):
                    return ProcessHandle(p.pid)
            time.sleep(0.5)
        loader.kill()
        raise RuntimeError(f"{exe} for {name} did not start within 60 s")
