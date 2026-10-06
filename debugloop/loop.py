import argparse
import subprocess
import sys
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from . import analyze, build, config, deploy, playsession, run, scenario, signature
from .ledger import Ledger
from .state import LoopState, StateCorrupt, atomic_write

MAX_ATTEMPTS = 5
MAX_HARNESS_ERRORS = 3
ALLOWED = ("reborn/", "gamecontroller/")
FORBIDDEN = ("reborn/BB/",)
REQUIRED_BRANCH = "agent/autofix"
PHASE_ORDER = {"startup": 0, "playing": 1}
REGRESSION_SCENARIO = "s0-solo-dojo-smoke"   # re-run after every fix commit
NOTES_DIR = "docs/notes/"
OK, HARNESS, STOPPED, LADDER_DONE, GAVE_UP, FIX_NEEDED, ATTEMPT_FAILED = 0, 2, 3, 4, 5, 10, 11


def _git(args: list[str]) -> str:
    return subprocess.run(["git", *args], cwd=config.REPO, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=True).stdout


def _branch() -> str:
    return _git(["rev-parse", "--abbrev-ref", "HEAD"]).strip()


_preconditions = run.preconditions   # the Deps field named `run` hides the module inside the class


@dataclass
class Deps:
    run: Callable = field(default=run.run_scenario)
    build: Callable = field(default=build.build_mod)
    deploy: Callable = field(default=deploy.deploy)
    triage: Callable = field(default=analyze.write_triage)
    git: Callable = field(default=_git)
    state_dir: Path = config.STATE_DIR
    runs_dir: Path = config.RUNS_DIR
    branch: Callable = field(default=_branch)
    preconditions: Callable = field(default=_preconditions)
    play_alive: Callable = field(default=playsession.alive)
    play_start: Callable = field(default=playsession.start)
    play_stop: Callable = field(default=playsession.stop)


def _load(d):
    return LoopState.load(d.state_dir), Ledger.load(d.state_dir)


def _entries(d) -> list[tuple[str, list[str]]]:
    """Parse `git status --porcelain -z` into (XY, [paths]); renames/copies list dest AND source."""
    parts = d.git(["status", "--porcelain", "-z"]).split(chr(0))
    out, i = [], 0
    while i < len(parts):
        e = parts[i]
        i += 1
        if len(e) < 4:
            continue
        xy, paths = e[:2], [e[3:]]
        if ("R" in xy or "C" in xy) and i < len(parts):
            paths.append(parts[i])
            i += 1
        out.append((xy, paths))
    return out


def _changed_paths(d) -> list[str]:
    """Every path a fix attempt changed. Untracked files outside reborn/ and gamecontroller/
    (the loop's own runs/state, stray files) are not a fix attempt and are ignored."""
    return [p for xy, paths in _entries(d) for p in paths
            if xy != "??" or p.startswith(ALLOWED)]


def _tracked_dirty(d) -> list[str]:
    return [p for xy, paths in _entries(d) if xy != "??" for p in paths]


def _forbidden(d) -> list[str]:
    return [p for p in _changed_paths(d) if not p.startswith(ALLOWED) or p.startswith(FORBIDDEN)]


def _has_changes(d) -> bool:
    return any(p.startswith(ALLOWED) for p in _changed_paths(d))


def _touched(d) -> list[str]:
    """Paths to stash or commit: never debugloop/, which is the loop's own folder."""
    return [p for p in _changed_paths(d) if not p.startswith("debugloop/")]


def _debugloop_edits(d) -> list[str]:
    return [p for p in _tracked_dirty(d) if p.startswith("debugloop/")]


def _head(d) -> str:
    return d.git(["rev-parse", "HEAD"]).strip()


def _head_moved(st, d) -> int | None:
    """Only the loop commits while a bug is open. Any other commit (a fixer that committed)
    would slip past every check, so a human looks first."""
    if st.bug_head is None:
        return None
    head = _head(d)
    if head == st.bug_head:
        return None
    print(f"STOPPED: needs a human: HEAD moved from {st.bug_head[:12]} to {head[:12]} while bug "
          f"{st.current_bug} was open; something other than the loop committed. Check the new "
          "commits, then run `python -m debugloop.loop reset-stop` to accept HEAD as it is now.")
    return STOPPED


def _restore_head_dll(d) -> None:
    """Build HEAD and deploy it, so a failed attempt's DLL never stays in the game folder.
    Failing here is only a warning: `next` builds and deploys HEAD again before it runs."""
    try:
        b = d.build()
        if not b.ok:
            print(f"WARNING: could not rebuild HEAD; `next` will retry.\n{b.output[-1500:]}")
            return
        d.deploy()
    except Exception as e:   # the attempt is already recorded; never lose that over this
        print(f"WARNING: could not put HEAD's DLL back in the game folder: {e}. `next` will retry.")


def _check_preconditions(st, d, scns) -> int | None:
    try:
        d.preconditions(d.runs_dir, max(len(s.processes) for s in scns))
    except run.HarnessError as e:
        return _harness_error(st, d, str(e))
    return None


def _deploy_or_harness(st, d) -> int | None:
    try:
        d.deploy()
    except deploy.DeployError as e:
        return _harness_error(st, d, f"deploy failed: {e}")
    return None


def _guard_branch(d):
    try:
        b = d.branch()
    except Exception as e:
        print(f"STOPPED: cannot read the git branch: {e}")
        return STOPPED
    if b != REQUIRED_BRANCH:
        print(f"STOPPED: on branch '{b}', not '{REQUIRED_BRANCH}'. Switch branch by hand.")
        return STOPPED
    return None


def _guard_play(d):
    """A live play session owns the game processes and debug ports: no test may start meanwhile.
    Not a harness error (the count stays), just a refusal."""
    info = d.play_alive()
    if info is None:
        return None
    print(f"a play session is running ({info.get('scenario', '?')}, ports {info.get('ports')}); "
          "end it with `python -m debugloop.loop stop-play` before the loop runs a test")
    return HARNESS


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


def _write_brief(d, st, bug, last_failure: str | None = None) -> None:
    """Written fresh each time, from the ledger: the run folder is the bug's latest failed run."""
    run_dir = d.runs_dir / bug.runs[-1]
    leftovers = [p for p in _changed_paths(d) if p.startswith(ALLOWED)]
    lines = [
        "# Fix brief", "",
        f"- Bug: `{bug.signature}` (seen {bug.count} times in {', '.join(bug.scenarios)})",
        f"- Attempt: {st.attempts_on_current + 1} of {MAX_ATTEMPTS}",
        f"- Run folder (latest failed run of this bug): `{run_dir}`",
        f"- Triage (read first): `{run_dir / 'triage.md'}`",
        f"- Earlier attempts: diffs in `{d.state_dir / 'attempts' / signature.slug(bug.signature)}`",
        f"- Also read every file in {NOTES_DIR} if present (`{config.REPO / NOTES_DIR}`).", ""]
    if leftovers:
        lines += ["## Warning: uncommitted changes already in the tree", "",
                  "These files were already changed when this brief was written, probably left by "
                  "an earlier fixer. They are not part of HEAD. Check them before you build on them:", "",
                  *[f"- `{p}`" for p in leftovers[:20]], ""]
    lines += [
        "## History", "", *[f"- {n}" for n in bug.notes], "",
        "## Rules", "",
        "- Edit only `reborn/` (never `reborn/BB/`) and `gamecontroller/`.",
        "- Do not commit, do not start the game, do not touch the game folder.",
        "- Build with `python -m debugloop.loop build` until it prints BUILD OK.",
        f"- Write 2-5 lines in `{d.state_dir / 'attempt_note.md'}`: what you changed and why.",
        "- Then stop. The loop verifies your fix by running the game.", ""]
    if last_failure:
        lines += [f"## Attempt {st.attempts_on_current} failed", "", last_failure, "",
                  f"Now on attempt {st.attempts_on_current + 1} of {MAX_ATTEMPTS}. Try a different idea.", ""]
    atomic_write(d.state_dir / "brief.md", "\n".join(lines))


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
    st.bug_phase, st.bug_elapsed_s, st.bug_milestone = r.outcome.phase, r.elapsed_s, r.milestone
    st.bug_head = _head(d)
    d.triage(r)
    _write_brief(d, st, bug)
    st.save(d.state_dir)
    print(f"FIX NEEDED: {bug.signature}  brief: {d.state_dir / 'brief.md'}")
    return FIX_NEEDED


def _close_bug(st) -> None:
    st.current_bug = st.bug_scenario = st.bug_phase = st.bug_head = None
    st.attempts_on_current, st.bug_elapsed_s, st.bug_milestone = 0, 0.0, 0


def _regression_run(d):
    """Step 0's smoke test after a fix commit. Returns (failed run or None, harness error or None)."""
    try:
        r = d.run(scenario.find_scenario(REGRESSION_SCENARIO))
    except (run.HarnessError, scenario.ScenarioError) as e:
        return None, str(e)
    if r.outcome.kind == "pass":
        return None, None
    if not r.signature:
        return None, f"run {r.run_id} ended '{r.outcome.kind}' with no signature"
    return r, None


def _revert_fix(st, L, d, fix: str, r, note: str) -> int:
    """The fix broke step 0: undo it with a new commit (never a reset) and count a failed attempt."""
    diff = d.git(["diff", f"{fix}~1", fix, "--", *ALLOWED])
    d.git(["revert", "--no-edit", fix])
    st.bug_head = _head(d)
    _record_failed_run(d, L, r)
    triage = d.triage(r)
    print(f"REGRESSION: the fix broke {REGRESSION_SCENARIO}; reverted {fix[:12]}")
    return _attempt_failed(st, L, d, f"fix broke step 0 smoke: {r.signature} in run {r.run_id} "
                                     f"(triage: {triage}). Note was: {note}", diff=diff)


def _commit_fix(st, L, d, subject: str, note: str, ran) -> tuple[bool, int | None, str, str | None]:
    """Commit the fix and check step 0 still passes. `ran` holds the scenarios this verify already
    ran and judged, so step 0's smoke test is not run (and judged) twice.
    Returns (committed, revert_rc, note, smoke_err). revert_rc is set when the fix broke step 0 and
    was reverted: the caller returns it as is. committed is False when there was nothing to commit."""
    if not _has_changes(d):
        return False, None, note, None
    d.git(["add", "-A", "--", *[p for p in _touched(d) if p.startswith(ALLOWED)]])
    d.git(["commit", "-m", f"{subject}\n\n{note}\n\n"
                           "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"])
    fix = st.bug_head = _head(d)
    st.save(d.state_dir)
    smoke_err = None
    if REGRESSION_SCENARIO not in ran:
        broke, smoke_err = _regression_run(d)
        if broke is not None:
            return True, _revert_fix(st, L, d, fix, broke, note), note, None
        if smoke_err:
            note += f" (step 0 smoke check could not run: {smoke_err})"
    return True, None, note, smoke_err


def _fixed(st, L, d, note: str, ran=frozenset()) -> int:
    """Commit the fix, check step 0 still passes, close the bug."""
    sig = st.current_bug
    committed, rc, note, smoke_err = _commit_fix(st, L, d, f"fix: {sig} [bb-autofix]", note, ran)
    if rc is not None:
        return rc
    if not committed:
        note += " (passed with no code change - may be flaky)"
    L.set_status(sig, "fixed", note)
    L.save()
    _close_bug(st)
    st.save(d.state_dir)
    _clear_note(d)
    print(f"FIXED {sig}")
    if smoke_err:
        return _harness_error(st, d, f"step 0 smoke check after the fix could not run: {smoke_err}")
    return OK


def _progress_on_same_bug(st, L, d, r, note: str, ran) -> int | None:
    """The run failed with the same signature but joined further (a higher milestone). Commit the
    fix, keep the bug open with a fresh attempt budget. None when there is no change to commit:
    the caller then counts an ordinary failed attempt."""
    if not _has_changes(d):
        return None
    sig, old = st.current_bug, st.bug_milestone
    committed, rc, note, smoke_err = _commit_fix(
        st, L, d, f"fix: {sig} progress to milestone {r.milestone} [bb-autofix]", note, ran)
    if rc is not None:
        return rc
    st.attempts_on_current = 0
    st.bug_milestone, st.bug_phase, st.bug_elapsed_s = r.milestone, r.outcome.phase, r.elapsed_s
    _record_failed_run(d, L, r)
    L.set_status(sig, "fixing", f"progress: reached milestone {r.milestone} in run {r.run_id}; "
                                f"attempts reset. Fix note: {note}")
    L.save()
    d.triage(r)
    _write_brief(d, st, L.get(sig))
    st.save(d.state_dir)
    _clear_note(d)
    print(f"PROGRESS {sig}: milestone {old} -> {r.milestone}; committed {st.bug_head[:12]}, attempts reset")
    if smoke_err:
        return _harness_error(st, d, f"step 0 smoke check after the fix could not run: {smoke_err}")
    return FIX_NEEDED


def _attempt_failed(st, L, d, why: str, diff: str | None = None) -> int:
    sig = st.current_bug
    st.attempts_on_current += 1
    L.get(sig).attempts += 1
    L.set_status(sig, "fixing", f"attempt {st.attempts_on_current} failed: {why}")
    folder = d.state_dir / "attempts" / signature.slug(sig)
    folder.mkdir(parents=True, exist_ok=True)
    diff_file = folder / f"{st.attempts_on_current}.diff"
    n = 2
    while diff_file.exists():   # never overwrite an earlier diff
        diff_file = folder / f"{st.attempts_on_current}-{n}.diff"
        n += 1
    if diff is None:
        diff = d.git(["diff", "HEAD", "--", *ALLOWED])
    diff_file.write_text(diff, encoding="utf-8")
    touched = _touched(d)
    if touched:
        # Only the paths the attempt touched: never sweep up debugloop/ or the loop's own state.
        d.git(["stash", "push", "-u", "-m", f"bb-autofix failed attempt {st.attempts_on_current} {sig}",
               "--", *touched])
    _clear_note(d)
    if st.attempts_on_current >= MAX_ATTEMPTS:
        L.set_status(sig, "gave_up", f"gave up after {MAX_ATTEMPTS} attempts")
        L.save()
        _close_bug(st)
        st.save(d.state_dir)
        _restore_head_dll(d)
        print(f"GAVE UP {sig}")
        return GAVE_UP
    L.save()
    st.save(d.state_dir)
    _write_brief(d, st, L.get(sig), why)
    _restore_head_dll(d)
    print(f"ATTEMPT FAILED ({st.attempts_on_current}/{MAX_ATTEMPTS}): {why[:300]}")
    return ATTEMPT_FAILED


def cmd_next(d: Deps) -> int:
    blocked = _guard_branch(d) or _guard_play(d)
    if blocked is not None:
        return blocked
    st, L = _load(d)
    if st.stopped_reason:
        print(f"STOPPED: {st.stopped_reason}")
        return STOPPED
    if st.current_bug:
        if st.bug_head is None:   # bug opened by a loop from before bug_head existed
            st.bug_head = _head(d)
            st.save(d.state_dir)
        print(f"FIX NEEDED: {st.current_bug}  brief: {d.state_dir / 'brief.md'}")
        return FIX_NEEDED
    scns = scenario.ladder(st.step)
    scn = scns[st.scenario_index % len(scns)]
    # Test HEAD, never a DLL a failed attempt left in the game folder. MSBuild only rebuilds
    # what changed, and deploy copies nothing when the deployed DLL already matches the build.
    blocked = _check_preconditions(st, d, [scn])
    if blocked is not None:
        return blocked
    b = d.build()
    if not b.ok:
        return _harness_error(st, d, f"building HEAD failed: {b.output[-1500:]}")
    blocked = _deploy_or_harness(st, d)
    if blocked is not None:
        return blocked
    try:
        r = d.run(scn)
    except run.HarnessError as e:
        return _harness_error(st, d, str(e))
    if r.outcome.kind != "pass" and not r.signature:
        return _harness_error(st, d, f"run {r.run_id} ended '{r.outcome.kind}' with no signature")
    st.harness_errors_in_row = 0
    if r.outcome.kind != "pass":
        dirty = _tracked_dirty(d)
        if dirty:
            print(f"STOPPED: the working tree has uncommitted changes {dirty[:5]}; "
                  "commit or stash them before the loop opens a bug.")
            return STOPPED
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


def _record_failed_run(d, L, r) -> None:
    """Keep the bug's history and its dump limit for runs that did not open a new bug."""
    _prune_dumps(d, L.record(r.signature, r.scenario, r.run_id))


def _smoke_of(scn):
    if scn.smoke:
        return None
    try:
        return scenario.find_scenario(scn.name + "-smoke")
    except scenario.ScenarioError:
        return None


def cmd_verify(d: Deps) -> int:
    blocked = _guard_branch(d) or _guard_play(d)
    if blocked is not None:
        return blocked
    st, L = _load(d)
    if not st.current_bug:
        print("nothing to verify")
        return OK
    moved = _head_moved(st, d)
    if moved is not None:
        return moved
    edited = _debugloop_edits(d)
    if edited:
        print(f"STOPPED: needs a human: debugloop/ was edited during a fix ({edited[:5]}). "
              "The fixer may only edit reborn/ and gamecontroller/. Nothing was stashed.")
        return STOPPED
    note = _note(d)
    bad = _forbidden(d)
    if bad:
        return _attempt_failed(st, L, d, f"touched forbidden paths {bad}. {note}")
    try:
        scn = scenario.find_scenario(st.bug_scenario)
    except scenario.ScenarioError as e:
        return _harness_error(st, d, str(e))
    to_run = [x for x in (_smoke_of(scn), scn) if x]
    blocked = _check_preconditions(st, d, to_run)
    if blocked is not None:
        return blocked
    b = d.build()
    if not b.ok:
        return _attempt_failed(st, L, d, f"build failed: {b.output[-1500:]}")
    blocked = _deploy_or_harness(st, d)
    if blocked is not None:
        return blocked
    r, ran = None, set()
    for s in to_run:
        ran.add(s.name)
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
        return _fixed(st, L, d, note, ran)
    # Join milestones first (a multi-stage bug can fail sooner each time yet get further), then the
    # outcome phase, then time. A bug opened before bug_milestone existed reads 0.
    later = (r.milestone, PHASE_ORDER.get(r.outcome.phase, 0), r.elapsed_s) > \
            (st.bug_milestone, PHASE_ORDER.get(st.bug_phase or "startup", 0), st.bug_elapsed_s)
    if r.signature != st.current_bug and later:
        rc = _fixed(st, L, d, f"{note} (got further; next failure {r.signature})", ran)
        if rc != OK:
            return rc
        return _new_failure(st, L, d, r.scenario, r)
    if r.signature == st.current_bug and r.milestone > st.bug_milestone:
        # A timeout keeps the same signature however far the join got, so the milestone is the
        # only sign of progress. Time or phase alone do not count.
        rc = _progress_on_same_bug(st, L, d, r, note, ran)
        if rc is not None:
            return rc
    _record_failed_run(d, L, r)
    triage = d.triage(r)
    if r.signature == st.current_bug:
        return _attempt_failed(st, L, d, f"same bug again in run {r.run_id}. Note was: {note}")
    return _attempt_failed(st, L, d, f"new, earlier failure {r.signature} in run {r.run_id} "
                                     f"(triage: {triage}). Note was: {note}")


def cmd_giveup(d: Deps) -> int:
    blocked = _guard_branch(d)
    if blocked is not None:
        return blocked
    st, L = _load(d)
    if not st.current_bug:
        return OK
    moved = _head_moved(st, d)
    if moved is not None:
        return moved
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
    if st.current_bug:
        st.bug_head = _head(d)   # accept HEAD as it is now (a checked commit, a harness repair)
    st.save(d.state_dir)
    return OK


def cmd_build(d: Deps) -> int:
    r = d.build()
    print("BUILD OK" if r.ok else "BUILD FAILED\n" + r.output[-6000:])
    return OK if r.ok else HARNESS


def cmd_play(d: Deps, name: str | None) -> int:
    """Start a scenario and leave it running for live driving (see playsession.py)."""
    if not name:
        print("usage: python -m debugloop.loop play <scenario>")
        return HARNESS
    blocked = _guard_branch(d) or _guard_play(d)
    if blocked is not None:
        return blocked
    try:
        scn = scenario.find_scenario(name)
    except scenario.ScenarioError as e:
        print(f"HARNESS ERROR: {e}")
        return HARNESS
    b = d.build()
    if not b.ok:
        print("BUILD FAILED\n" + b.output[-3000:])
        return HARNESS
    try:
        d.deploy()
        info = d.play_start(scn)
    except (run.HarnessError, deploy.DeployError, OSError) as e:
        print(f"HARNESS ERROR: {e}")
        return HARNESS
    print(f"play session up: {scn.name}  run folder {info['run_dir']}")
    for n, p in info["ports"].items():
        print(f"  {n:8} port {p}")
    print("drive it with the battleborn-play MCP tools or `python -m debugloop.play ...`; "
          "end it with `python -m debugloop.loop stop-play`")
    return OK


def cmd_stop_play(d: Deps) -> int:
    killed = d.play_stop()
    print("stopped: " + ", ".join(killed) if killed else "no play session was running")
    return OK


def _state_corrupt(e: StateCorrupt) -> int:
    print(f"STOPPED: {e} The loop never resets its own state; a human has to repair or move "
          "the file before the loop can go on.")
    return STOPPED


def main(argv=None, deps: Deps | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m debugloop.loop")
    ap.add_argument("command", choices=["next", "verify", "giveup", "status", "reset-stop", "build",
                                        "play", "stop-play"])
    ap.add_argument("scenario", nargs="?", help="play: the scenario to start")
    a = ap.parse_args(argv)
    fn = {"next": cmd_next, "verify": cmd_verify, "giveup": cmd_giveup, "status": cmd_status,
          "reset-stop": cmd_reset_stop, "build": cmd_build,
          "play": lambda d: cmd_play(d, a.scenario), "stop-play": cmd_stop_play}[a.command]
    d = deps or Deps()
    try:
        return fn(d)
    except StateCorrupt as e:
        return _state_corrupt(e)
    except Exception as e:
        # Anything unexpected is a harness error, so three in a row stop the loop.
        traceback.print_exc(file=sys.stdout)
        try:
            st = LoopState.load(d.state_dir)
        except StateCorrupt as e2:
            return _state_corrupt(e2)
        return _harness_error(st, d, f"unexpected Python error (traceback above): "
                                     f"{type(e).__name__}: {e}")


if __name__ == "__main__":
    sys.exit(main())
