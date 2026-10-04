from pathlib import Path

import pytest

from debugloop import deploy, loop, run
from debugloop.build import BuildResult
from debugloop.ledger import Ledger
from debugloop.outcome import Outcome
from debugloop.run import RunResult
from debugloop.state import LoopState


class FakeGit:
    def __init__(self, status=" M reborn/Hooks.cpp\n"):
        self.calls, self.status = [], status

    def __call__(self, args):
        self.calls.append(args)
        if args[0] == "status":
            return self.status
        if args[0] == "diff":
            return "the diff"
        return ""

    def did(self, verb):
        return any(c[0] == verb for c in self.calls)


def deps(tmp_path, outcomes, build_ok=True, status=" M reborn/Hooks.cpp\n"):
    it = iter(outcomes)

    def fake_run(scn):
        o = next(it)
        if isinstance(o, Exception):
            raise o
        kind, sig, phase, elapsed = o
        rd = tmp_path / "runs" / f"r{elapsed}"
        rd.mkdir(parents=True, exist_ok=True)
        return RunResult(f"r{elapsed}", scn.name, Outcome(kind, phase=phase), sig, rd, elapsed)

    return loop.Deps(run=fake_run, build=lambda: BuildResult(build_ok, "compiler said no", None),
                     deploy=lambda: None, triage=lambda r: r.run_dir / "triage.md",
                     git=FakeGit(status), state_dir=tmp_path / "state", runs_dir=tmp_path / "runs")


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
    d = deps(tmp_path, [BUG], status=" M reborn/BB/SDK_HEADERS/Engine_classes.hpp\n")
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
    status = " M reborn/Hooks.cpp\n" + "?? debugloop/state/x.json\n"
    d = deps(tmp_path, [BUG, BUG], status=status)
    loop.cmd_next(d)
    loop.cmd_verify(d)
    stash = next(c for c in d.git.calls if c[0] == "stash")
    assert stash[-1] == "reborn/Hooks.cpp" and "debugloop/state/x.json" not in stash


def test_no_signature_failure_is_harness_error(tmp_path):
    d = deps(tmp_path, [("exit", None, "playing", 5)])
    assert loop.cmd_next(d) == 2
    assert LoopState.load(d.state_dir).current_bug is None
