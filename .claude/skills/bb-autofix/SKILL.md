---
name: bb-autofix
description: One iteration of the Battleborn autonomous debug loop - run the game test, and if it finds a bug, have a Fable subagent fix it and verify. Run as `/loop /bb-autofix`.
---

# bb-autofix: one loop iteration

Work from the repo root `C:\Users\djsin\Documents\GitHub\Battleborn-Server\reborn`, on branch `agent/autofix`. Never push. Never change Windows settings. Never close a game window the loop did not start.

## 1. Run the next test

Run `python -m debugloop.loop next` (timeout: 45 minutes; run it in the background and wait for the notification if your tool limit is shorter). Act on the exit code:

| Exit | Meaning | Do |
|---|---|---|
| 0 | test passed | If the output says `NOW AT STEP`, send a push notification: "Battleborn loop reached step N". End the iteration. |
| 2 | harness error | Read the message. If it says a game is already running, push-notify the user once ("close the game so the loop can continue") and end the iteration. Otherwise end the iteration (the loop retries). |
| 3 | stopped | Read `python -m debugloop.loop status`. If the reason is harness errors with a Python traceback, go to section 4. If the reason is the wrong branch or uncommitted changes, push-notify the reason and stop the loop. Never switch branches, stash or commit to fix this yourself. Otherwise push-notify the reason and stop the loop. |
| 4 | ladder done | Push-notify "Steps 0-3 pass. Ready for human players." Stop the loop. |
| 10 | bug found | Go to section 2. |

## 2. Fix (Fable subagent)

Dispatch one subagent with the Agent tool, `model: "fable"`, `subagent_type: "general-purpose"`, foreground. Prompt:

> You are fixing one bug in the Battleborn Reborn mod (C++ DLL injected into a 2016 Unreal Engine 3 game) or its C# lobby server. Repo: `C:\Users\djsin\Documents\GitHub\Battleborn-Server\reborn`. Read `debugloop/state/brief.md` first, then the triage file it names, then the run folder logs. Use the superpowers:systematic-debugging skill: form a hypothesis from the evidence before editing. Follow every rule in the brief. Edit only files under `reborn/` (never `reborn/BB/`) or `gamecontroller/`. Never edit `debugloop/`. Do not commit; the loop commits. Make the smallest change that plausibly fixes the root cause; prefer guarding the exact failing path over broad rewrites. Build with `python -m debugloop.loop build` until BUILD OK. Write your note to `debugloop/state/attempt_note.md`. If you are certain the bug cannot be fixed from mod code, write why in the note and run `python -m debugloop.loop giveup`. Reply with 3 lines: hypothesis, change, confidence.

## 3. Verify

Run `python -m debugloop.loop verify` (timeout 45 minutes, background if needed).

| Exit | Do |
|---|---|
| 0 | Fixed and committed. End the iteration. |
| 10 | Progress: fixed one bug, a new later one is open. Go back to section 2 in this same iteration (at most 3 fix cycles per iteration). |
| 11 | Attempt failed (brief now has the reason). Go back to section 2 (at most 3 fix cycles per iteration). |
| 5 | Gave up on this bug. Push-notify "Gave up on bug <signature> after 5 tries". End the iteration. |
| 2 / 3 | As in section 1. |

## 4. Harness repair (Sonnet subagent)

Only when the loop stopped on harness errors with a Python traceback in `debugloop/` AND `git status --porcelain` shows no changes under `reborn/` or `gamecontroller/`. If there are uncommitted changes under `reborn/` or `gamecontroller/`, push-notify the user and stop the loop. Otherwise, dispatch a subagent, `model: "sonnet"`: "The test harness in `debugloop/` failed: <message>. Reproduce with pytest, fix `debugloop/` only, keep all tests passing (`python -m pytest debugloop/tests -v`), commit only files under `debugloop/`. Never touch `reborn/` or `gamecontroller/`. Use commit message `fix(debugloop): ...`." Then run `python -m debugloop.loop reset-stop` and end the iteration. If the same traceback happens again after a repair, push-notify the user and stop the loop.

## Every iteration ends with

One line to the user: step, passes in a row, open bug (if any), what happened.
