import os
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
MOD_PROJECT = REPO / "reborn" / "reborn.vcxproj"
MOD_DLL = REPO / "reborn" / "x64" / "Release" / "reborn.dll"
GAME_DIR = Path(os.environ.get("BB_GAME_DIR", r"C:\Users\djsin\Games\Battleborn"))
WIN64 = GAME_DIR / "Binaries" / "Win64"
PKG = Path(__file__).resolve().parent
LOG_DIR = Path(os.environ["USERPROFILE"]) / "Documents" / "RebornLogs"
# The game's own crash handler writes POPLAR-*.dmp here.
GAME_LOGS_DIR = Path(os.environ["USERPROFILE"]) / "Documents" / "My Games" / "Battleborn" / "PoplarGame" / "Logs"

MIN_FREE_MB = 2048
FIRST_DEBUG_PORT = 18080
SERVER_PORT = 7777
HANG_SECONDS = 60
