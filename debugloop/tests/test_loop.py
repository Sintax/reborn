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

    def __init__(self, status=" M reborn/Hooks.cpp\x00", state_dir=None, always_dirty=False):
        self.calls, self.status = [], status
        self.state_dir, self.always_dirty = state_dir, always_dirty

    def _bug_open(self):
        f = self.state_dir / "loop_state.json"
        return f.exists() and json.loads(f.read_text())["current_bug"] is not None

    def __call__(self, args):
        self.calls.append(args)
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

    def fake_run(scn):
        o = next(it)
        if isinstance(o, Exception):
            raise o
        kind, sig, phase, elapsed = o
        rid = f"r{next(n)}"
        rd = tmp_path / "runs" / rid
        rd.mkdir(parents=True, exist_ok=True)
        if kind != "pass":
            (rd / "crash.dmp").write_text("x")
        return RunResult(rid, scn.name, Outcome(kind, phase=phase), sig, rd, elapsed)

    return loop.Deps(run=fake_run, build=lambda: BuildResult(build_ok, "compiler said no", None),
                     deploy=lambda: None, triage=lambda r: r.run_dir / "triage.md",
                     git=FakeGit(status, tmp_path / "state", always_dirty),
                     state_dir=tmp_path / "state", runs_dir=tmp_path / "runs",
                     branch=lambda: branch)


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


def test_forbidden_path_rejected_before_build(tmp_path):
    d = deps(tmp_path, [BUG], status=" M reborn/BB/SDK_HEADERS/Engine_classes.hpp\x00")
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11
    assert "forbidden" in Ledger.load(d.state_dir).get("exit:3").notes[-1]


def test_build_failure_is_failed_attempt(tmp_path):
    d = deps(tmp_path, [BUG], build_ok=False)
    loop.cmd_next(d)
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


def test_debugloop_change_is_forbidden_and_not_stashed(tmp_path):
    status = " M reborn/Hooks.cpp\x00 M debugloop/loop.py\x00"
    d = deps(tmp_path, [BUG], status=status)
    loop.cmd_next(d)
    assert loop.cmd_verify(d) == 11
    assert "forbidden" in Ledger.load(d.state_dir).get("exit:3").notes[-1]
    stash = next(c for c in d.git.calls if c[0] == "stash")
    assert "debugloop/loop.py" not in stash and "reborn/Hooks.cpp" in stash


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
