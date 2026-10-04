import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MOD_PROJECT = REPO / "reborn" / "reborn.vcxproj"
MOD_DLL = REPO / "reborn" / "x64" / "Release" / "reborn.dll"
GAME_DIR = Path(os.environ.get("BB_GAME_DIR", r"D:\SteamLibrary\steamapps\common\Battleborn"))
WIN64 = GAME_DIR / "Binaries" / "Win64"
PKG = REPO / "debugloop"
RUNS_DIR = PKG / "runs"
STATE_DIR = PKG / "state"
SCENARIOS_DIR = PKG / "scenarios"
NATIVE_OUT = PKG / "native"
LOG_DIR = Path(os.environ["USERPROFILE"]) / "Documents" / "RebornLogs"

MIN_FREE_MB = 2048
FIRST_DEBUG_PORT = 18080
SERVER_PORT = 7777
HANG_SECONDS = 60
