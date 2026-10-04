import json
import subprocess
from pathlib import Path

import pytest

from debugloop import deploy, loop, run
from debugloop.build import BuildResult
from debugloop.ledger import Ledger
from debugloop.outcome import Outcome
from debugloop.run import RunResult
from debugloop.state import LoopState


class FakeGit:
    """`status` is what `git status --porcelain -z` shows while a fix is being attempted.
    The tree is clean until a bug is open, unless always_dirty is set."""

    def __init__(self, status=" M reborn/Hooks.cpp\x00", state_dir=None, always_dirty=False, log=None):
        self.calls, self.status = [], status
        self.state_dir, self.always_dirty = state_dir, always_dirty
        self.head, self.log = "h0", log if log is not None else []

    def _bug_open(self):
        f = self.state_dir / "loop_state.json"
        return f.exists() and json.loads(f.read_text())["current_bug"] is not None

    def __call__(self, args):
        self.calls.append(args)
        self.log.append(args[0])
        if args[0] == "rev-parse":
            return self.head + "\n"
        if args[0] in ("commit", "revert"):
            self.head = f"h{len(self.calls)}"
        if args[0] == "status":
            return self.status if self.always_dirty or self._bug_open() else ""
        if args[0] == "diff":
            return "the diff"
        return ""

    def did(self, verb):
        return any(c[0] == verb for c in self.calls)


def deps(tmp_path, outcomes, build_ok=True, status=" M reborn/Hooks.cpp\x00", branch="agent/autofix",
         always_dirty=False):
    it = iter(outcomes)
    n = iter(range(1000))
    log, ran, triaged = [], [], []

    def fake_run(scn):
        log.append("run")
        ran.append(scn.name)
        o = next(it)
        if isinstance(o, Exception):
            raise o
        kind, sig, phase, elapsed, *rest = o   # optional 5th item: the run's join milestone
        rid = f"r{next(n)}"
        rd = tmp_path / "runs" / rid
        rd.mkdir(parents=True, exist_ok=True)
        if kind != "pass":
            (rd / "crash.dmp").write_text("x")
        return RunResult(rid, scn.name, Outcome(kind, phase=phase), sig, rd, elapsed,
                         rest[0] if rest else 0)

    def fake_build():
        log.append("build")
        return BuildResult(build_ok, "compiler said no", None)

    def fake_triage(r):
        triaged.append(r.run_id)
        return r.run_dir / "triage.md"

    d = loop.Deps(run=fake_run, build=fake_build, deploy=lambda: log.append("deploy"),
                  triage=fake_triage, git=FakeGit(status, tmp_path / "state", always_dirty, log),
                  state_dir=tmp_path / "state", runs_dir=tmp_path / "runs",
                  branch=lambda: branch,
                  preconditions=lambda runs_dir, n: log.append("preconditions"))
    d.log, d.ran, d.triaged = log, ran, triaged
    return d


PASS = ("pass", None, "playing", 300)
BUG = ("exit", "exit:3", "playing", 100)


def test_three_passes_climb_to_step_1(tmp_path):
    d = deps(tmp_path, [PASS] * 3)
    assert [loop.cmd_next(d) for _ in range(3)] == [0, 0, 0]
    assert LoopState.load(d.state_dir).step == 1


def test_failure_opens_bug_and_writes_brief(tmp_path):
    d = deps(tmp_path, [BUG])
    assert loop.cmd_next(d) == 10
    st = LoopState.load(d.state_dir)
    assert st.current_bug == "exit:3" and st.bug_scenario == "s0-solo-dojo"
    assert "exit:3" in (d.state_dir / "brief.md").read_text()
    assert Ledger.load(d.state_dir).get("exit:3").status == "fixing"
    assert loop.cmd_next(d) == 10, "next refuses to run while a bug is open"


def test_verify_pass_commits_and_closes(tmp_path):
    d = deps(tmp_path, [BUG, PASS, PASS])
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 0
    assert d.git.did("commit")
    assert Ledger.load(d.state_dir).get("exit:3").status == "fixed"
    assert LoopState.load(d.state_dir).current_bug is None


def test_same_bug_five_times_gives_up_and_stashes(tmp_path):
    d = deps(tmp_path, [BUG] * 6)
    loop.cmd_next(d)
    codes = [loop.cmd_verify(d) for _ in range(5)]
    assert codes == [11, 11, 11, 11, 5]
    assert d.git.did("stash")
    assert (d.state_dir / "attempts" / "exit_3" / "1.diff").read_text() == "the diff"
    assert Ledger.load(d.state_dir).get("exit:3").status == "gave_up"


def test_later_different_failure_is_progress(tmp_path):
    d = deps(tmp_path, [BUG, ("crash", "crash:0xc0000005:battleborn+0x10", "playing", 200)])
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 10
    L = Ledger.load(d.state_dir)
    assert L.get("exit:3").status == "fixed"
    assert LoopState.load(d.state_dir).current_bug == "crash:0xc0000005:battleborn+0x10"


def test_earlier_different_failure_is_regression(tmp_path):
    d = deps(tmp_path, [BUG, ("timeout", "timeout:startup", "startup", 240)])
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11
    assert LoopState.load(d.state_dir).current_bug == "exit:3"


def test_later_milestone_beats_earlier_time(tmp_path):
    # Each join fix moved the failure further along, but it happened sooner than the old timeout.
    d = deps(tmp_path, [BUG, ("crash", "crash:0xc0000005:spawn", "startup", 50, 3)])
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 10
    assert d.git.did("commit")
    st = LoopState.load(d.state_dir)
    assert st.current_bug == "crash:0xc0000005:spawn"
    assert st.bug_milestone == 3 and st.bug_elapsed_s == 50
    assert Ledger.load(d.state_dir).get("exit:3").status == "fixed"


def test_same_milestone_earlier_time_is_still_regression(tmp_path):
    d = deps(tmp_path, [("exit", "exit:3", "playing", 100, 2), ("exit", "exit:4", "playing", 50, 2)])
    loop.cmd_next(d)
    assert LoopState.load(d.state_dir).bug_milestone == 2
    assert loop.cmd_verify(d) == 11
    assert LoopState.load(d.state_dir).current_bug == "exit:3"
    assert not d.git.did("commit")


def test_lower_milestone_later_time_is_regression(tmp_path):
    d = deps(tmp_path, [("exit", "exit:3", "playing", 100, 3), ("exit", "exit:4", "playing", 900, 1)])
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11


def test_old_bug_without_milestone_counts_any_progress_as_further(tmp_path):
    # The live bug was opened before bug_milestone existed: it loads as 0.
    d = deps(tmp_path, [("crash", "crash:0xc0000005:spawn", "startup", 50, 1), PASS])   # PASS: step 0 smoke
    LoopState(step=1, current_bug="timeout:startup", bug_scenario="s1-dojo-1client",
              bug_phase="startup", bug_elapsed_s=241.8, bug_head="h0").save(d.state_dir)
    L = Ledger.load(d.state_dir)
    L.record("timeout:startup", "s1-dojo-1client", "r-old")
    L.save()
    f = d.state_dir / "loop_state.json"
    old = json.loads(f.read_text())
    del old["bug_milestone"]
    f.write_text(json.dumps(old))
    assert "bug_milestone" not in (d.state_dir / "loop_state.json").read_text()
    d.git.status = " M reborn/Hooks.cpp\x00"
    assert loop.cmd_verify(d) == 10
    assert LoopState.load(d.state_dir).bug_milestone == 1


def test_forbidden_path_rejected_before_build(tmp_path):
    d = deps(tmp_path, [BUG], status=" M reborn/BB/SDK_HEADERS/Engine_classes.hpp\x00")
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11
    assert "forbidden" in Ledger.load(d.state_dir).get("exit:3").notes[-1]


def _fail_builds(d):
    d.build = lambda: BuildResult(False, "compiler said no", None)


def test_build_failure_is_failed_attempt(tmp_path):
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)
    _fail_builds(d)
    assert loop.cmd_verify(d) == 11
    assert "compiler said no" in Ledger.load(d.state_dir).get("exit:3").notes[-1]


def test_three_harness_errors_stop(tmp_path):
    d = deps(tmp_path, [run.HarnessError("boom")] * 3)
    assert [loop.cmd_next(d) for _ in range(3)] == [2, 2, 3]
    assert "boom" in LoopState.load(d.state_dir).stopped_reason
    assert loop.cmd_next(d) == 3
    assert loop.cmd_reset_stop(d) == 0
    assert LoopState.load(d.state_dir).stopped_reason is None


def test_given_up_bug_blocks_ladder(tmp_path):
    d = deps(tmp_path, [BUG] * 7)
    loop.cmd_next(d)
    for _ in range(5):
        loop.cmd_verify(d)
    assert loop.cmd_next(d) == 3


def test_deploy_error_is_harness_error_not_attempt(tmp_path):
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)

    def bad_deploy():
        raise deploy.DeployError("not the game folder")
    d.deploy = bad_deploy
    assert loop.cmd_verify(d) == 2
    st = LoopState.load(d.state_dir)
    assert st.attempts_on_current == 0 and st.harness_errors_in_row == 1


def test_failed_attempt_stashes_only_touched_paths(tmp_path):
    status = " M reborn/Hooks.cpp\x00?? debugloop/state/x.json\x00"
    d = deps(tmp_path, [BUG, BUG], status=status)
    loop.cmd_next(d)
    loop.cmd_verify(d)
    stash = next(c for c in d.git.calls if c[0] == "stash")
    assert stash[-1] == "reborn/Hooks.cpp" and "debugloop/state/x.json" not in stash


def test_wrong_branch_refuses_next_and_verify(tmp_path):
    d = deps(tmp_path, [PASS, BUG], branch="main")
    assert loop.cmd_next(d) == 3
    assert not (d.state_dir / "loop_state.json").exists(), "nothing ran"
    d2 = deps(tmp_path, [BUG])
    loop.cmd_next(d2)
    d2.branch = lambda: "main"
    assert loop.cmd_verify(d2) == 3
    assert LoopState.load(d2.state_dir).attempts_on_current == 0


def test_dirty_tree_refuses_to_open_bug(tmp_path):
    d = deps(tmp_path, [BUG], always_dirty=True)
    assert loop.cmd_next(d) == 3
    st = LoopState.load(d.state_dir)
    assert st.current_bug is None and st.stopped_reason is None
    assert not d.git.did("stash")


def test_staged_rename_out_of_sdk_is_forbidden(tmp_path):
    status = "R  reborn/X.hpp\x00reborn/BB/X.hpp\x00"
    d = deps(tmp_path, [BUG], status=status)
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11
    assert "forbidden" in Ledger.load(d.state_dir).get("exit:3").notes[-1]
    assert not d.git.did("commit")


def test_non_ascii_path_parsed(tmp_path):
    d = deps(tmp_path, [BUG], status=" M reborn/Hooks é日.cpp\x00?? docs/stray.txt\x00")
    loop.cmd_next(d)
    assert loop._changed_paths(d) == ["reborn/Hooks é日.cpp"]
    assert loop._forbidden(d) == []


def test_debugloop_edit_during_fix_needs_a_human(tmp_path, capsys):
    status = " M reborn/Hooks.cpp\x00 M debugloop/loop.py\x00"
    d = deps(tmp_path, [BUG], status=status)
    loop.cmd_next(d)
    d.log.clear()
    assert loop.cmd_verify(d) == 3
    assert "needs a human: debugloop/ was edited during a fix" in capsys.readouterr().out
    assert not d.git.did("stash") and "build" not in d.log and "run" not in d.log
    assert LoopState.load(d.state_dir).attempts_on_current == 0, "not a failed attempt"
    assert Ledger.load(d.state_dir).get("exit:3").attempts == 0


def test_untracked_debugloop_file_does_not_stop_verify(tmp_path):
    d = deps(tmp_path, [BUG, PASS, PASS], status=" M reborn/Hooks.cpp\x00?? debugloop/scratch.txt\x00")
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 0


def test_dumps_limited_to_three_after_failed_attempts(tmp_path):
    d = deps(tmp_path, [BUG] * 5)
    loop.cmd_next(d)
    for _ in range(4):
        assert loop.cmd_verify(d) == 11
    dumps = list((tmp_path / "runs").glob("*/*.dmp"))
    assert len(dumps) == 3
    assert Ledger.load(d.state_dir).get("exit:3").count == 5


def test_diff_never_overwritten(tmp_path):
    d = deps(tmp_path, [BUG, BUG])
    loop.cmd_next(d)
    folder = d.state_dir / "attempts" / "exit_3"
    folder.mkdir(parents=True)
    (folder / "1.diff").write_text("old")
    loop.cmd_verify(d)
    assert (folder / "1.diff").read_text() == "old"
    assert (folder / "1-2.diff").read_text() == "the diff"


def test_missing_scenario_in_verify_returns_2(tmp_path):
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)
    st = LoopState.load(d.state_dir)
    st.bug_scenario = "no-such-scenario"
    st.save(d.state_dir)
    assert loop.cmd_verify(d) == 2


def test_build_command_failure_returns_2(tmp_path):
    d = deps(tmp_path, [], build_ok=False)
    assert loop.cmd_build(d) == 2


def test_real_git_pathspec_stash_in_throwaway_repo(tmp_path):
    def git(*a):
        return subprocess.run(["git", *a], cwd=tmp_path, capture_output=True, text=True,
                              encoding="utf-8", check=True).stdout
    git("init", "-q")
    for n in ("user.email", "user.name"):
        git("config", n, "t")
    (tmp_path / "reborn").mkdir()
    (tmp_path / "reborn" / "a.cpp").write_text("a")
    git("add", "-A")
    git("commit", "-qm", "i")
    (tmp_path / "reborn" / "a.cpp").write_text("b")
    git("mv", "reborn/a.cpp", "reborn/é.cpp")
    d = loop.Deps(git=lambda args: git(*args))
    assert sorted(loop._changed_paths(d)) == ["reborn/a.cpp", "reborn/é.cpp"]


def test_no_signature_failure_is_harness_error(tmp_path):
    d = deps(tmp_path, [("exit", None, "playing", 5)])
    assert loop.cmd_next(d) == 2
    assert LoopState.load(d.state_dir).current_bug is None


# --- final review fixes -------------------------------------------------------------------


def test_next_builds_and_deploys_head_before_running(tmp_path):
    d = deps(tmp_path, [PASS])
    assert loop.cmd_next(d) == 0
    assert d.log == ["preconditions", "build", "deploy", "run"]


def test_next_build_failure_is_harness_error(tmp_path):
    d = deps(tmp_path, [PASS], build_ok=False)
    assert loop.cmd_next(d) == 2
    assert "run" not in d.log and "deploy" not in d.log
    assert LoopState.load(d.state_dir).harness_errors_in_row == 1


def test_next_deploy_failure_is_harness_error(tmp_path):
    d = deps(tmp_path, [PASS])

    def bad_deploy():
        raise deploy.DeployError("reborn.dll is locked")
    d.deploy = bad_deploy
    assert loop.cmd_next(d) == 2
    assert "run" not in d.log
    assert LoopState.load(d.state_dir).harness_errors_in_row == 1


def test_next_precondition_failure_comes_before_build(tmp_path):
    d = deps(tmp_path, [PASS])

    def busy(runs_dir, n):
        raise run.HarnessError("a game process is already running")
    d.preconditions = busy
    assert loop.cmd_next(d) == 2
    assert d.log == []


def test_failed_attempt_puts_head_dll_back(tmp_path):
    d = deps(tmp_path, [BUG, BUG])
    loop.cmd_next(d)
    d.log.clear()
    assert loop.cmd_verify(d) == 11
    after = d.log[d.log.index("stash"):]
    assert "build" in after and after.index("build") < after.index("deploy"), d.log


def test_giveup_puts_head_dll_back(tmp_path):
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)
    d.log.clear()
    assert loop.cmd_giveup(d) == 5
    after = d.log[d.log.index("stash"):]
    assert "build" in after and "deploy" in after


def test_giveup_refuses_wrong_branch(tmp_path):
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)
    d.branch = lambda: "main"
    assert loop.cmd_giveup(d) == 3
    assert LoopState.load(d.state_dir).current_bug == "exit:3"
    assert not d.git.did("stash")


def test_foreign_game_at_verify_exits_2_before_any_deploy(tmp_path, monkeypatch):
    from debugloop import launch
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)
    d.log.clear()
    d.preconditions = run.preconditions          # the real checks, with a fake game process
    monkeypatch.setattr(launch, "find_game_processes", lambda: [424242])
    assert loop.cmd_verify(d) == 2
    assert "deploy" not in d.log and "build" not in d.log and "run" not in d.log
    st = LoopState.load(d.state_dir)
    assert st.harness_errors_in_row == 1 and st.attempts_on_current == 0


def test_unexpected_exception_in_main_is_harness_error(tmp_path, capsys):
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)

    def locked():
        raise PermissionError(13, "Permission denied", "reborn.dll")
    d.deploy = locked
    assert loop.main(["verify"], deps=d) == 2
    out = capsys.readouterr().out
    assert "Traceback" in out and "PermissionError" in out
    st = LoopState.load(d.state_dir)
    assert st.harness_errors_in_row == 1 and st.current_bug == "exit:3"


def test_three_unexpected_exceptions_stop_the_loop(tmp_path):
    d = deps(tmp_path, [])

    def boom():
        raise PermissionError(13, "Permission denied", "reborn.dll")
    d.deploy = boom
    assert [loop.main(["next"], deps=d) for _ in range(3)] == [2, 2, 3]
    assert "PermissionError" in LoopState.load(d.state_dir).stopped_reason


def test_corrupt_state_stops_and_is_never_reset(tmp_path, capsys):
    d = deps(tmp_path, [PASS])
    d.state_dir.mkdir(parents=True)
    f = d.state_dir / "loop_state.json"
    f.write_bytes(b"{half \xff")
    for cmd in ("next", "verify", "giveup", "reset-stop"):
        assert loop.main([cmd], deps=d) == 3
    assert "loop_state.json" in capsys.readouterr().out
    assert f.read_bytes() == b"{half \xff"
    assert "run" not in d.log


def test_corrupt_ledger_stops(tmp_path):
    d = deps(tmp_path, [PASS])
    d.state_dir.mkdir(parents=True)
    (d.state_dir / "ledger.json").write_text("{half")
    assert loop.main(["next"], deps=d) == 3
    assert (d.state_dir / "ledger.json").read_text() == "{half"


def test_head_recorded_when_bug_opens_and_after_commit(tmp_path):
    d = deps(tmp_path, [BUG, ("crash", "crash:0xc0000005:battleborn+0x10", "playing", 200)])
    loop.cmd_next(d)
    assert LoopState.load(d.state_dir).bug_head == "h0"
    assert loop.cmd_verify(d) == 10            # fix committed, later bug opened
    assert LoopState.load(d.state_dir).bug_head == d.git.head != "h0"


def test_head_moved_stops_verify_and_giveup(tmp_path, capsys):
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)
    d.git.head = "fixer-commit"
    assert loop.cmd_verify(d) == 3
    assert loop.cmd_giveup(d) == 3
    assert "HEAD moved" in capsys.readouterr().out
    st = LoopState.load(d.state_dir)
    assert st.attempts_on_current == 0 and st.current_bug == "exit:3"
    assert not d.git.did("stash") and not d.git.did("commit")


def test_reset_stop_accepts_head_as_it_is_now(tmp_path):
    d = deps(tmp_path, [BUG, BUG])
    loop.cmd_next(d)
    d.git.head = "harness-repair-commit"
    assert loop.cmd_reset_stop(d) == 0
    assert LoopState.load(d.state_dir).bug_head == "harness-repair-commit"
    assert loop.cmd_verify(d) == 11


def test_next_records_head_for_a_bug_opened_by_an_older_loop(tmp_path):
    d = deps(tmp_path, [])
    LoopState(step=1, current_bug="timeout:startup", bug_scenario="s1-dojo-1client",
              attempts_on_current=1).save(d.state_dir)
    assert loop.cmd_next(d) == 10
    assert LoopState.load(d.state_dir).bug_head == "h0"


def test_missing_scenario_in_verify_counts_as_harness_error(tmp_path):
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)
    st = LoopState.load(d.state_dir)
    st.bug_scenario = "no-such-scenario"
    st.save(d.state_dir)
    assert [loop.cmd_verify(d) for _ in range(3)] == [2, 2, 3]


def test_brief_points_at_latest_failed_run_and_docs_notes(tmp_path):
    d = deps(tmp_path, [BUG, BUG])
    loop.cmd_next(d)
    brief = (d.state_dir / "brief.md").read_text()
    assert str(tmp_path / "runs" / "r0") in brief
    assert loop.cmd_verify(d) == 11
    brief = (d.state_dir / "brief.md").read_text()
    assert str(tmp_path / "runs" / "r1") in brief and str(tmp_path / "runs" / "r0") not in brief
    assert str(tmp_path / "runs" / "r1" / "triage.md") in brief
    assert "Attempt 1 failed" in brief and "same bug again" in brief
    assert "Attempt: 2 of 5" in brief and "read every file in docs/notes/ if present" in brief


def test_failed_verify_runs_get_triage(tmp_path):
    d = deps(tmp_path, [BUG, BUG, ("timeout", "timeout:startup", "startup", 240)])
    loop.cmd_next(d)
    loop.cmd_verify(d)
    loop.cmd_verify(d)
    assert d.triaged == ["r0", "r1", "r2"]


def test_brief_warns_about_leftover_changes(tmp_path):
    d = deps(tmp_path, [BUG], status="?? reborn/Leftover.cpp\x00", always_dirty=True)
    assert loop.cmd_next(d) == 10
    brief = (d.state_dir / "brief.md").read_text()
    assert "uncommitted" in brief and "reborn/Leftover.cpp" in brief


def test_clean_tree_brief_has_no_leftover_warning(tmp_path):
    d = deps(tmp_path, [BUG])
    loop.cmd_next(d)
    assert "uncommitted" not in (d.state_dir / "brief.md").read_text()


def _step1(d):
    LoopState(step=1).save(d.state_dir)


def test_fix_that_breaks_step0_smoke_is_reverted(tmp_path):
    d = deps(tmp_path, [BUG, PASS, PASS, ("exit", "exit:7", "playing", 30)])
    _step1(d)
    assert loop.cmd_next(d) == 10
    d.log.clear()
    assert loop.cmd_verify(d) == 11
    assert d.ran[-1] == "s0-solo-dojo-smoke"
    fix_commit = next(i for i, c in enumerate(d.git.calls) if c[0] == "commit")
    revert = next(c for c in d.git.calls if c[0] == "revert")
    assert revert[:2] == ["revert", "--no-edit"] and not d.git.did("reset")
    assert fix_commit < d.git.calls.index(revert)
    after = d.log[d.log.index("revert"):]
    assert "build" in after and "deploy" in after, "HEAD's DLL goes back in the game folder"
    st = LoopState.load(d.state_dir)
    assert st.current_bug == "exit:3" and st.attempts_on_current == 1
    assert st.bug_head == d.git.head
    bug = Ledger.load(d.state_dir).get("exit:3")
    assert bug.status == "fixing" and "fix broke step 0 smoke" in bug.notes[-1]


def test_fix_that_keeps_step0_smoke_passing_is_kept(tmp_path):
    d = deps(tmp_path, [BUG, PASS, PASS, PASS])
    _step1(d)
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 0
    assert d.ran[-1] == "s0-solo-dojo-smoke"
    assert not d.git.did("revert")
    assert Ledger.load(d.state_dir).get("exit:3").status == "fixed"


def test_step0_smoke_not_rerun_when_verify_just_passed_it(tmp_path):
    d = deps(tmp_path, [BUG, PASS, PASS])
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 0
    assert d.ran == ["s0-solo-dojo", "s0-solo-dojo-smoke", "s0-solo-dojo"]


def test_step0_check_that_cannot_run_keeps_the_fix_and_counts_harness_error(tmp_path):
    d = deps(tmp_path, [BUG, PASS, PASS, run.HarnessError("port busy")])
    _step1(d)
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 2
    assert d.git.did("commit") and not d.git.did("revert")
    st = LoopState.load(d.state_dir)
    assert st.current_bug is None and st.harness_errors_in_row == 1
    assert "could not run" in Ledger.load(d.state_dir).get("exit:3").notes[-1]


def test_progress_whose_fix_breaks_step0_is_reverted_and_no_new_bug_opens(tmp_path):
    d = deps(tmp_path, [BUG, ("crash", "crash:0xc0000005:battleborn+0x10", "playing", 200),
                        ("exit", "exit:7", "playing", 30)])
    _step1(d)
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11
    assert d.git.did("revert")
    assert LoopState.load(d.state_dir).current_bug == "exit:3"


# --- same bug, but the join got further (a timeout keeps its signature at every milestone) ---

def _opened_at_milestone(tmp_path, outcomes, milestone=1, **kw):
    """Step 1, bug exit:3 opened at `milestone`; `outcomes` are the verify runs (smoke first)."""
    d = deps(tmp_path, [("exit", "exit:3", "startup", 240, milestone), *outcomes], **kw)
    _step1(d)
    assert loop.cmd_next(d) == 10
    return d


def test_same_bug_higher_milestone_is_progress(tmp_path, capsys):
    d = _opened_at_milestone(tmp_path, [PASS, ("exit", "exit:3", "startup", 240, 4), PASS])
    st = LoopState.load(d.state_dir)
    scn, st.attempts_on_current = st.bug_scenario, 3
    st.save(d.state_dir)
    capsys.readouterr()
    assert loop.cmd_verify(d) == 10
    commit = next(c for c in d.git.calls if c[0] == "commit")
    assert "fix: exit:3 progress to milestone 4 [bb-autofix]" in commit[2]
    assert commit[2].rstrip().endswith("Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>")
    assert not d.git.did("stash") and not d.git.did("revert")
    st = LoopState.load(d.state_dir)
    assert st.current_bug == "exit:3" and st.bug_scenario == scn
    assert st.attempts_on_current == 0 and st.bug_milestone == 4
    assert st.bug_phase == "startup" and st.bug_elapsed_s == 240
    assert st.bug_head == d.git.head
    bug = Ledger.load(d.state_dir).get("exit:3")
    assert bug.status == "fixing" and "progress: reached milestone 4 in run" in bug.notes[-1]
    assert "attempts reset" in bug.notes[-1]
    assert "Attempt: 1 of 5" in (d.state_dir / "brief.md").read_text()
    assert not (d.state_dir / "attempt_note.md").exists()
    assert d.triaged[-1] == bug.runs[-1]
    assert f"PROGRESS exit:3: milestone 1 -> 4; committed {d.git.head[:12]}, attempts reset" in \
        capsys.readouterr().out


def test_same_bug_progress_keeps_the_scenario_and_the_next_verify_works(tmp_path):
    d = _opened_at_milestone(tmp_path, [PASS, ("exit", "exit:3", "startup", 240, 4), PASS,
                                        PASS, PASS, PASS])
    scn = LoopState.load(d.state_dir).bug_scenario
    assert loop.cmd_verify(d) == 10
    assert LoopState.load(d.state_dir).bug_scenario == scn
    assert loop.cmd_verify(d) == 0
    assert Ledger.load(d.state_dir).get("exit:3").status == "fixed"


def test_same_bug_same_milestone_later_time_is_still_a_failed_attempt(tmp_path):
    d = _opened_at_milestone(tmp_path, [PASS, ("exit", "exit:3", "playing", 900, 1)], milestone=1)
    assert loop.cmd_verify(d) == 11
    assert not d.git.did("commit")
    st = LoopState.load(d.state_dir)
    assert st.attempts_on_current == 1 and st.bug_milestone == 1


def test_same_bug_lower_milestone_is_a_failed_attempt(tmp_path):
    d = _opened_at_milestone(tmp_path, [PASS, ("exit", "exit:3", "playing", 900, 1)], milestone=3)
    assert loop.cmd_verify(d) == 11
    assert not d.git.did("commit")


def test_same_bug_higher_milestone_whose_fix_breaks_step0_is_reverted(tmp_path):
    d = _opened_at_milestone(tmp_path, [PASS, ("exit", "exit:3", "startup", 240, 4),
                                        ("exit", "exit:7", "playing", 30)])
    assert loop.cmd_verify(d) == 11
    assert d.git.did("commit") and d.git.did("revert")
    st = LoopState.load(d.state_dir)
    assert st.current_bug == "exit:3" and st.attempts_on_current == 1
    assert st.bug_milestone == 1, "a reverted fix does not move the bug's milestone"
    assert st.bug_head == d.git.head
    bug = Ledger.load(d.state_dir).get("exit:3")
    assert bug.status == "fixing" and "fix broke step 0 smoke" in bug.notes[-1]


def test_same_bug_progress_on_the_last_attempt_is_not_a_give_up(tmp_path):
    d = _opened_at_milestone(tmp_path, [PASS, ("exit", "exit:3", "startup", 240, 4), PASS])
    st = LoopState.load(d.state_dir)
    st.attempts_on_current = loop.MAX_ATTEMPTS - 1
    st.save(d.state_dir)
    assert loop.cmd_verify(d) == 10
    st = LoopState.load(d.state_dir)
    assert st.current_bug == "exit:3" and st.attempts_on_current == 0
    assert Ledger.load(d.state_dir).get("exit:3").status == "fixing"


def test_same_bug_higher_milestone_with_no_change_to_commit_is_a_failed_attempt(tmp_path):
    d = _opened_at_milestone(tmp_path, [PASS, ("exit", "exit:3", "startup", 240, 4)])
    d.git.status = ""
    assert loop.cmd_verify(d) == 11
    assert not d.git.did("commit")
    st = LoopState.load(d.state_dir)
    assert st.attempts_on_current == 1 and st.bug_milestone == 1


def test_same_bug_progress_whose_step0_check_cannot_run_is_a_harness_error(tmp_path):
    d = _opened_at_milestone(tmp_path, [PASS, ("exit", "exit:3", "startup", 240, 4),
                                        run.HarnessError("port busy")])
    assert loop.cmd_verify(d) == 2
    assert d.git.did("commit") and not d.git.did("revert")
    st = LoopState.load(d.state_dir)
    assert st.current_bug == "exit:3" and st.attempts_on_current == 0 and st.bug_milestone == 4
    assert st.harness_errors_in_row == 1
