"""Release numbers: 1.0.N, where N is the build. Build 0 is a developer build."""
import re
from pathlib import Path

from .config import REPO

BASE = "1.0"
_RELEASE = re.compile(r"^1\.0\.([1-9][0-9]*)$", re.ASCII)
HEADER = REPO / "reborn" / "BuildVersion.hpp"


def parse(version: str) -> int:
    """'1.0.7' -> 7. A git SHA or anything else -> 0 (a developer build)."""
    m = _RELEASE.match((version or "").strip())
    return int(m.group(1)) if m else 0


def version_string(build: int) -> str:
    return f"{BASE}.{build}" if build > 0 else "dev"


def next_build(tags: list[str]) -> int:
    """Highest 1.0.<n> tag + 1; the first release is 1."""
    return max((parse(t) for t in tags), default=0) + 1


def write_header(build: int, path: Path = HEADER) -> None:
    path.write_text(
        "// Written by tools/version.py. Not in git.\n"
        f'#define REBORN_VERSION "{version_string(build)}"\n'
        f"#define REBORN_BUILD {max(build, 0)}\n",
        encoding="utf-8")
