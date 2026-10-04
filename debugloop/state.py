import json
import os
from dataclasses import asdict, dataclass
from pathlib import Path

from . import config

FILE = "loop_state.json"


class StateCorrupt(RuntimeError):
    pass


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


@dataclass
class LoopState:
    step: int = 0
    consecutive_passes: int = 0
    scenario_index: int = 0
    current_bug: str | None = None
    bug_scenario: str | None = None
    attempts_on_current: int = 0
    harness_errors_in_row: int = 0
    stopped_reason: str | None = None
    bug_phase: str | None = None          # phase the current bug happened in
    bug_elapsed_s: float = 0.0            # how far into the run it happened

    @classmethod
    def load(cls, directory: Path = config.STATE_DIR) -> "LoopState":
        p = directory / FILE
        if not p.exists():
            return cls()
        try:
            return cls(**json.loads(p.read_text(encoding="utf-8")))
        except (json.JSONDecodeError, TypeError) as e:
            raise StateCorrupt(f"{p} is unreadable: {e}. Fix or delete it by hand.") from e

    def save(self, directory: Path = config.STATE_DIR) -> None:
        atomic_write(directory / FILE, json.dumps(asdict(self), indent=2))
