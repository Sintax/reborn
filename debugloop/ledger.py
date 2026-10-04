import json
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path

from . import config
from .state import atomic_write


def _now() -> str:
    return datetime.now().isoformat(timespec="seconds")


@dataclass
class Bug:
    signature: str
    first_seen: str
    last_seen: str
    count: int = 0
    scenarios: list[str] = field(default_factory=list)
    status: str = "open"
    attempts: int = 0
    runs: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)


class Ledger:
    def __init__(self, directory: Path, bugs: dict[str, Bug]):
        self.dir = directory
        self.bugs = bugs

    @classmethod
    def load(cls, directory: Path = config.STATE_DIR) -> "Ledger":
        p = directory / "ledger.json"
        bugs = {}
        if p.exists():
            bugs = {k: Bug(**v) for k, v in json.loads(p.read_text(encoding="utf-8")).items()}
        return cls(directory, bugs)

    def get(self, sig: str) -> Bug | None:
        return self.bugs.get(sig)

    def record(self, sig: str, scenario: str, run_id: str) -> Bug:
        b = self.bugs.get(sig)
        if b is None:
            b = self.bugs[sig] = Bug(sig, _now(), _now())
        if b.status == "fixed":
            b.status = "open"
            b.notes.append(f"{_now()} reopened by {run_id}")
        b.count += 1
        b.last_seen = _now()
        if scenario not in b.scenarios:
            b.scenarios.append(scenario)
        b.runs.append(run_id)
        return b

    def set_status(self, sig: str, status: str, note: str = "") -> None:
        b = self.bugs[sig]
        b.status = status
        if note:
            b.notes.append(f"{_now()} {status}: {note}")

    def open_bugs(self) -> list[Bug]:
        return sorted((b for b in self.bugs.values() if b.status in ("open", "fixing")),
                      key=lambda b: -b.count)

    def save(self) -> None:
        atomic_write(self.dir / "ledger.json",
                     json.dumps({k: asdict(v) for k, v in self.bugs.items()}, indent=2))
        lines = ["# Bug ledger", "", "| Status | Count | Signature | Scenarios | Last seen |",
                 "|---|---|---|---|---|"]
        for b in sorted(self.bugs.values(), key=lambda b: (b.status, -b.count)):
            lines.append(f"| {b.status} | {b.count} | `{b.signature}` | "
                         f"{', '.join(b.scenarios)} | {b.last_seen} |")
        atomic_write(self.dir / "ledger.md", "\n".join(lines) + "\n")
