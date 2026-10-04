import argparse
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from . import analyze, build, config, deploy, run, scenario, signature
from .ledger import Ledger
from .state import LoopState, atomic_write

MAX_ATTEMPTS = 5
MAX_HARNESS_ERRORS = 3
ALLOWED = ("reborn/", "gamecontroller/")
FORBIDDEN = ("reborn/BB/",)
PHASE_ORDER = {"startup": 0, "playing": 1}
OK, HARNESS, STOPPED, LADDER_DONE, GAVE_UP, FIX_NEEDED, ATTEMPT_FAILED = 0, 2, 3, 4, 5, 10, 11


def _git(args: list[str]) -> str:
    return subprocess.run(["git", *args], cwd=config.REPO, capture_output=True, text=True,
                          check=True).stdout


@dataclass
class Deps:
    run: Callable = field(default=run.run_scenario)
    build: Callable = field(default=build.build_mod)
    deploy: Callable = field(default=deploy.deploy)
    triage: Callable = field(default=analyze.write_triage)
    git: Callable = field(default=_git)
    state_dir: Path = config.STATE_DIR
    runs_dir: Path = config.RUNS_DIR


def _load(d):
    return LoopState.load(d.state_dir), Ledger.load(d.state_dir)


def _changed_paths(d) -> list[str]:
    paths = []
    for line in d.git(["status", "--porcelain"]).splitlines():
        if line.strip():
            paths.append(line[3:].split(" -> ")[-1].strip().strip('"'))
    return paths


def _forbidden(d) -> list[str]:
    return [p for p in _changed_paths(d)
            if not p.startswith(("debugloop/",)) and (not p.startswith(ALLOWED) or p.startswith(FORBIDDEN))]


def _has_changes(d) -> bool:
    return any(p.startswith(ALLOWED) for p in _changed_paths(d))


def _touched(d) -> list[str]:
    """Paths a fix attempt changed (never debugloop/, which is the loop's own folder)."""
    return [p for p in _changed_paths(d) if not p.startswith("debugloop/")]


def _note(d) -> str:
    p = d.state_dir / "attempt_note.md"
    return p.read_text(encoding="utf-8").strip() if p.exists() else "(no note)"


def _clear_note(d) -> None:
    (d.state_dir / "attempt_note.md").unlink(missing_ok=True)


def _harness_error(st, d, msg: str) -> int:
    st.harness_errors_in_row += 1
    if st.harness_errors_in_row >= MAX_HARNESS_ERRORS:
        st.stopped_reason = f"{MAX_HARNESS_ERRORS} harness errors in a row; last: {msg}"
        st.save(d.state_dir)
        print(f"STOPPED: {st.stopped_reason}")
        return STOPPED
    st.save(d.state_dir)
    print(f"HARNESS ERROR ({st.harness_errors_in_row}/{MAX_HARNESS_ERRORS}): {msg}")
    return HARNESS


def _prune_dumps(d, bug) -> None:
    for run_id in bug.runs[:-3]:
        for dmp in (d.runs_dir / run_id).glob("*.dmp"):
            dmp.unlink()


def _write_brief(d, st, bug, r, triage: Path) -> None:
    atomic_write(d.state_dir / "brief.md", "\n".join([
        "# Fix brief", "",
        f"- Bug: `{bug.signature}` (seen {bug.count} times in {', '.join(bug.scenarios)})",
        f"- Attempt: {st.attempts_on_current + 1} of {MAX_ATTEMPTS}",
        f"- Run folder: `{r.run_dir}`",
        f"- Triage (read first): `{triage}`",
        f"- Earlier attempts: diffs in `{d.state_dir / 'attempts' / signature.slug(bug.signature)}`",
        "", "## History", "", *[f"- {n}" for n in bug.notes], "",
        "## Rules", "",
        "- Edit only `reborn/` (never `reborn/BB/`) and `gamecontroller/`.",
        "- Do not commit, do not start the game, do not touch the game folder.",
        "- Build with `python -m debugloop.loop build` until it prints BUILD OK.",
        f"- Write 2-5 lines in `{d.state_dir / 'attempt_note.md'}`: what you changed and why.",
        "- Then stop. The loop verifies your fix by running the game.", ""]))


def _new_failure(st, L, d, scn_name: str, r) -> int:
    bug = L.record(r.signature, scn_name, r.run_id)
    _prune_dumps(d, bug)
    st.consecutive_passes = 0
    if bug.status == "gave_up":
        L.save()
        st.stopped_reason = f"blocked by a bug the AI gave up on: {bug.signature}"
        st.save(d.state_dir)
        print(f"STOPPED: {st.stopped_reason}")
        return STOPPED
    L.set_status(bug.signature, "fixing")
    L.save()
    st.current_bug, st.bug_scenario, st.attempts_on_current = bug.signature, scn_name, 0
    st.bug_phase, st.bug_elapsed_s = r.outcome.phase, r.elapsed_s
    _write_brief(d, st, bug, r, d.triage(r))
    st.save(d.state_dir)
    print(f"FIX NEEDED: {bug.signature}  brief: {d.state_dir / 'brief.md'}")
    return FIX_NEEDED


def _close_bug(st) -> None:
    st.current_bug = st.bug_scenario = st.bug_phase = None
    st.attempts_on_current, st.bug_elapsed_s = 0, 0.0


def _fixed(st, L, d, note: str) -> None:
    sig = st.current_bug
    if _has_changes(d):
        d.git(["add", "-A", "--", *[p for p in _touched(d) if p.startswith(ALLOWED)]])
        d.git(["commit", "-m", f"fix: {sig} [bb-autofix]\n\n{note}\n\n"
                               "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"])
    else:
        note += " (passed with no code change - may be flaky)"
    L.set_status(sig, "fixed", note)
    L.save()
    _close_bug(st)
    st.save(d.state_dir)
    _clear_note(d)
    print(f"FIXED {sig}")


def _attempt_failed(st, L, d, why: str) -> int:
    sig = st.current_bug
    st.attempts_on_current += 1
    L.get(sig).attempts += 1
    L.set_status(sig, "fixing", f"attempt {st.attempts_on_current} failed: {why}")
    folder = d.state_dir / "attempts" / signature.slug(sig)
    folder.mkdir(parents=True, exist_ok=True)
    (folder / f"{st.attempts_on_current}.diff").write_text(d.git(["diff", "HEAD", "--", *ALLOWED]))
    if _has_changes(d) or _forbidden(d):
        # Only the paths the attempt touched: never sweep up the loop's own state files.
        d.git(["stash", "push", "-u", "-m", f"bb-autofix failed attempt {st.attempts_on_current} {sig}",
               "--", *_touched(d)])
    _clear_note(d)
    if st.attempts_on_current >= MAX_ATTEMPTS:
        L.set_status(sig, "gave_up", f"gave up after {MAX_ATTEMPTS} attempts")
        L.save()
        _close_bug(st)
        st.save(d.state_dir)
        print(f"GAVE UP {sig}")
        return GAVE_UP
    L.save()
    st.save(d.state_dir)
    with open(d.state_dir / "brief.md", "a", encoding="utf-8") as f:
        f.write(f"\n## Attempt {st.attempts_on_current} failed\n\n{why}\n\n"
                f"Now on attempt {st.attempts_on_current + 1} of {MAX_ATTEMPTS}. Try a different idea.\n")
    print(f"ATTEMPT FAILED ({st.attempts_on_current}/{MAX_ATTEMPTS}): {why[:300]}")
    return ATTEMPT_FAILED


def cmd_next(d: Deps) -> int:
    st, L = _load(d)
    if st.stopped_reason:
        print(f"STOPPED: {st.stopped_reason}")
        return STOPPED
    if st.current_bug:
        print(f"FIX NEEDED: {st.current_bug}  brief: {d.state_dir / 'brief.md'}")
        return FIX_NEEDED
    scns = scenario.ladder(st.step)
    scn = scns[st.scenario_index % len(scns)]
    try:
        r = d.run(scn)
    except run.HarnessError as e:
        return _harness_error(st, d, str(e))
    if r.outcome.kind != "pass" and not r.signature:
        return _harness_error(st, d, f"run {r.run_id} ended '{r.outcome.kind}' with no signature")
    st.harness_errors_in_row = 0
    if r.outcome.kind != "pass":
        return _new_failure(st, L, d, scn.name, r)
    st.consecutive_passes += 1
    st.scenario_index += 1
    msg = f"PASS {scn.name} ({st.consecutive_passes}/{scn.required_passes} in a row at step {st.step})"
    if st.consecutive_passes >= scn.required_passes:
        st.step, st.consecutive_passes, st.scenario_index = st.step + 1, 0, 0
        msg += f" -> NOW AT STEP {st.step}"
    if st.step > 3:
        st.stopped_reason = "steps 0-3 complete; step 4 needs human players"
    st.save(d.state_dir)
    print(msg)
    return LADDER_DONE if st.step > 3 else OK


def _smoke_of(scn):
    if scn.smoke:
        return None
    try:
        return scenario.find_scenario(scn.name + "-smoke")
    except scenario.ScenarioError:
        return None


def cmd_verify(d: Deps) -> int:
    st, L = _load(d)
    if not st.current_bug:
        print("nothing to verify")
        return OK
    note = _note(d)
    bad = _forbidden(d)
    if bad:
        return _attempt_failed(st, L, d, f"touched forbidden paths {bad}. {note}")
    b = d.build()
    if not b.ok:
        return _attempt_failed(st, L, d, f"build failed: {b.output[-1500:]}")
    try:
        d.deploy()
    except deploy.DeployError as e:
        return _harness_error(st, d, f"deploy failed: {e}")
    scn = scenario.find_scenario(st.bug_scenario)
    r = None
    for s in [x for x in (_smoke_of(scn), scn) if x]:
        try:
            r = d.run(s)
        except run.HarnessError as e:
            return _harness_error(st, d, str(e))
        if r.outcome.kind != "pass" and not r.signature:
            return _harness_error(st, d, f"run {r.run_id} ended '{r.outcome.kind}' with no signature")
        if r.outcome.kind != "pass":
            break
    st.harness_errors_in_row = 0
    if r.outcome.kind == "pass":
        _fixed(st, L, d, note)
        return OK
    if r.signature == st.current_bug:
        return _attempt_failed(st, L, d, f"same bug again in run {r.run_id}. Note was: {note}")
    later = (PHASE_ORDER.get(r.outcome.phase, 0), r.elapsed_s) > \
            (PHASE_ORDER.get(st.bug_phase or "startup", 0), st.bug_elapsed_s)
    if later:
        _fixed(st, L, d, f"{note} (got further; next failure {r.signature})")
        return _new_failure(st, L, d, r.scenario, r)
    return _attempt_failed(st, L, d, f"new, earlier failure {r.signature} in run {r.run_id}. Note was: {note}")


def cmd_giveup(d: Deps) -> int:
    st, L = _load(d)
    if not st.current_bug:
        return OK
    st.attempts_on_current = MAX_ATTEMPTS - 1
    return _attempt_failed(st, L, d, "fix agent gave up")


def cmd_status(d: Deps) -> int:
    st, _ = _load(d)
    print(st)
    md = d.state_dir / "ledger.md"
    print(md.read_text(encoding="utf-8") if md.exists() else "(no bugs yet)")
    return OK


def cmd_reset_stop(d: Deps) -> int:
    st, _ = _load(d)
    st.stopped_reason, st.harness_errors_in_row = None, 0
    st.save(d.state_dir)
    return OK


def cmd_build(d: Deps) -> int:
    r = d.build()
    print("BUILD OK" if r.ok else "BUILD FAILED\n" + r.output[-6000:])
    return OK if r.ok else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m debugloop.loop")
    ap.add_argument("command", choices=["next", "verify", "giveup", "status", "reset-stop", "build"])
    a = ap.parse_args(argv)
    fn = {"next": cmd_next, "verify": cmd_verify, "giveup": cmd_giveup, "status": cmd_status,
          "reset-stop": cmd_reset_stop, "build": cmd_build}[a.command]
    return fn(Deps())


if __name__ == "__main__":
    sys.exit(main())
