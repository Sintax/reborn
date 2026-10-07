---
name: bb-autofix
description: One iteration of the Battleborn autonomous debug loop - run the game test, and if it finds a bug, have the bb-fixer subagent fix it and verify. Run as `/loop /bb-autofix`.
---

# bb-autofix: one loop iteration

To stop the loop: if running under `/loop` dynamic mode, call ScheduleWakeup with stop: true; otherwise end the iteration and tell the user the loop is stopped and why.

Work from the repo root `C:\Users\djsin\Documents\GitHub\Battleborn-Server\reborn`, on branch `agent/autofix`. Never push. Never change Windows settings. Never close a game window the loop did not start.

## Combat brain: off while basics are broken

The user's rule: the smart player controls (the combat brain, `-rbcombat` in the client args of `debugloop/scenarios/s1-*`, `s2-*`, `s3-*`) are an extra failure point. If basic bugs keep coming back (invisible players, a client stuck in character select or never getting a pawn, startup timeouts, or similar join/spawn bugs across several attempts), remove `-rbcombat` from those scenarios so the ladder tests only the server's main functions. Commit only those scenario files (`fix(debugloop): ...`), then run `python -m debugloop.loop reset-stop` so the loop accepts the new HEAD.

Once the ladder passes cleanly without it, put `-rbcombat` back on the clients and let the ladder run again. If the basic bugs return with it on, turn it off again and tell the user.

Current setting: OFF (since 2026-10-07, while `invisible:IceScort_P` and character-select timeouts kept recurring).

## 1. Run the next test

Run `python -m debugloop.loop next` in the background with a 110-minute timeout, and wait for the notification. It builds and deploys the current code before the test, so it can take a while. Act on the exit code:

| Exit | Meaning | Do |
|---|---|---|
| 0 | test passed | If the output says `NOW AT STEP`, send a push notification: "Battleborn loop reached step N". End the iteration. |
| 2 | harness error | Read the message. If it says a game is already running, push-notify the user once ("close the game so the loop can continue") and end the iteration. Otherwise end the iteration (the loop retries). |
| 3 | stopped | Read the `STOPPED:` line in the command output. For harness-error stops it is also shown by `python -m debugloop.loop status`. If the reason is harness errors with a Python traceback, go to section 4. If the reason is the wrong branch or uncommitted changes, push-notify the reason and stop the loop. Never switch branches, stash or commit to fix this yourself. Otherwise push-notify the reason and stop the loop. |
| 4 | ladder done | Push-notify "Steps 0-3 pass. Ready for human players." Stop the loop. |
| 6 | a play session is still running | If you started it in this iteration (section 1b), run `python -m debugloop.loop stop-play` and run `next` again. Otherwise someone is playing it: push-notify "A Battleborn play session is still running; the loop is paused until it ends (`python -m debugloop.loop stop-play`)" and stop the loop. |
| 10 | bug found | Go to section 2. |
| any other exit code | unexpected | Push-notify the last 20 lines of the output and stop the loop. |

## 1b. Live reproduction (optional, combat bugs only)

The ladder's players fight with the combat brain (`-rbcombat`). When the brief's signature, triage or "Combat warnings" mention damage, weapon, skill, death, respawn or firing, and this is the bug's first attempt, you may reproduce it by hand before dispatching the fixer:

1. `python -m debugloop.loop play <scenario from the brief>` (background, 15-minute timeout). It builds, deploys, starts the games and prints each player's name and port.
2. Use the `battleborn-play` tools (`bb_list`, `bb_situation`, `bb_order`, `bb_act`, `bb_log`; `bb_look` only when the text leaves a doubt, pictures cost far more) for at most 10 minutes to steer the players into the situation, for example `bb_order c1 hunt` and `bb_order c2 follow` with c1's id. Without the MCP tools, `python -m debugloop.play ...` does the same.
3. Append what you saw, with the exact log lines, under a `## Live reproduction` heading at the end of `debugloop/state/brief.md`.
4. `python -m debugloop.loop stop-play`. `next` and `verify` refuse to run while a play session is up.

Skip this for startup, join or map-load failures; the loop's own run already shows those.

## 2. Fix (bb-fixer subagent: Opus, extra-high effort)

Dispatch one subagent with the Agent tool, `subagent_type: "bb-fixer"`, foreground, with no `model` override. Its rules live in `.claude/agents/bb-fixer.md`.

Prompt: "Fix the bug in `debugloop/state/brief.md`." Then add short controller context:
- what the last run's logs showed (quote the key lines);
- which earlier attempt diff to start from, if any (`debugloop/state/attempts/<bug>/`, newest by time);
- the current milestone and what the next one looks like.

Escalation: if the same stage has failed 3 attempts in a row and the last run's logs taught nothing new, dispatch the next attempt with `model: "fable"` (same subagent type). Use Fable only for that one attempt, then go back to the default.

If the fixer reports it ran `giveup` and the output said `STOPPED`, handle it like exit 3 in section 1. If the fixer reports it ran `giveup` otherwise, push-notify "Gave up on bug <signature>: <reason from attempt_note.md>" and end the iteration without running verify.

## 3. Verify

Run `python -m debugloop.loop verify` in the background with a 110-minute timeout, and wait for the notification.

| Exit | Do |
|---|---|
| 0 | Fixed and committed. End the iteration. |
| 10 | Progress: either fixed one bug and a new later one is open, or the same bug got further (the output says `PROGRESS`: the fix was committed, the join reached a higher milestone, and the attempt count is reset). Go back to section 2 in this same iteration (at most 2 fix cycles per iteration). |
| 11 | Attempt failed (brief now has the reason). Go back to section 2 (at most 2 fix cycles per iteration). |
| 5 | Gave up on this bug. Push-notify "Gave up on bug <signature> after 5 tries". End the iteration. |
| 2 / 3 / 6 | As in section 1. |
| any other exit code | Push-notify the last 20 lines of the output and stop the loop. |

## 4. Harness repair (Sonnet subagent)

Only when the loop stopped on harness errors with a Python traceback in `debugloop/` AND `git status --porcelain` shows no changes under `reborn/` or `gamecontroller/`. If there are uncommitted changes under `reborn/` or `gamecontroller/`, push-notify the user and stop the loop. Otherwise, dispatch a subagent, `model: "sonnet"`: "The test harness in `debugloop/` failed: <message>. Reproduce with pytest, fix `debugloop/` only, keep all tests passing (`python -m pytest debugloop/tests -v`), commit only files under `debugloop/`. Never touch `reborn/` or `gamecontroller/`. Use commit message `fix(debugloop): ...`." Then run `python -m debugloop.loop reset-stop` and end the iteration. If the same traceback happens again after a repair, push-notify the user and stop the loop.

## Every iteration ends with

One line to the user: step, passes in a row, open bug (if any), what happened.

## Usage budget

Each fix attempt costs a lot of usage. Run at most 2 fix cycles per iteration, then schedule the next iteration instead of continuing. If the user says they are near their usage limit, finish the current step and stop the loop; the loop's saved state lets the next session pick up where this one stopped.
