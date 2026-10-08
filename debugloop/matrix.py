"""Hero matrix: every hero in the co-op story mission with two players, before co-op over the
internet. Each pair runs s2-algorithm-2clients-smoke with its two heroes swapped in, and must pass
like any test: the picked hero loads (wronghero), in its own skin (wrongskin), both bodies drawn
(invisible), no falling through the world (fell), no crash, hang or disconnect.

A separate sweep: it never reads or writes the loop's ladder state or ledger. Results go to
state/matrix.json and state/matrix.md.

    python -m debugloop.matrix [--heroes A,B,...] [--rounds N] [--resume]

Exit 0 when every pair passed, 10 when any failed, 2 on a harness problem (or pairs left unrun),
6 while a play session holds the games.
"""
import argparse
import json
import re
import sys
import tempfile
import traceback
from dataclasses import asdict
from datetime import datetime
from pathlib import Path

from . import config, deploy, loop, netem, run, scenario
from .state import atomic_write

BASE_SCENARIO = "s2-algorithm-2clients-smoke"
CONSTANTS = config.REPO / "reborn" / "Constants.hpp"
MAX_HARNESS_ERRORS = 3
OK, HARNESS, PLAY_SESSION, FAILED = 0, 2, 6, 10


def all_heroes(path: Path = CONSTANTS) -> list[str]:
    """The heroes -rbcharacter takes: Constants::CharacterSelectCharacterTable, in its order."""
    text = path.read_text(encoding="utf-8")
    m = re.search(r"CharacterSelectCharacterTable\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise ValueError(f"no CharacterSelectCharacterTable in {path}")
    return re.findall(r'"([^"]+)"', m.group(1))


def slug(hero: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", hero.lower()).strip("-")


def pair_name(c1: str, c2: str) -> str:
    return f"matrix-{slug(c1)}-{slug(c2)}"


def pairs_for_round(heroes: list[str], rnd: int) -> list[tuple[str, str]]:
    """(c1 hero, c2 hero) pairs that play every hero at least once. Round 0 pairs neighbours in
    list order (A0-B0, A1-B1, ...). Round r pairs each A with the B r places on, so partners differ
    from earlier rounds (while r < number of pairs), and odd rounds swap who is c1 and c2. An odd
    count borrows the first hero to fill the last pair."""
    hs = list(heroes)
    if len(hs) < 2:
        raise ValueError("the matrix needs at least two heroes")
    if len(hs) % 2:
        hs.append(next(h for h in hs if h != hs[-1]))
    a, b = hs[0::2], hs[1::2]
    k = len(a)
    out = []
    for i in range(k):
        x, y = a[i], b[(i + rnd) % k]
        if x == y:   # only possible with the borrowed hero: take the next B instead
            y = b[(i + rnd + 1) % k]
        out.append((y, x) if rnd % 2 else (x, y))
    return out


def plan(heroes: list[str], rounds: int) -> list[dict]:
    """Every pair of every round, in run order, each once (a repeat of an earlier pair is dropped)."""
    seen, out = set(), []
    for rnd in range(rounds):
        for c1, c2 in pairs_for_round(heroes, rnd):
            name = pair_name(c1, c2)
            if name not in seen:
                seen.add(name)
                out.append({"name": name, "round": rnd + 1, "c1": c1, "c2": c2})
    return out


def _toml_str(v) -> str:
    return json.dumps(v)   # a JSON string is a valid TOML basic string


def scenario_text(base: scenario.Scenario, name: str, heroes: list[str],
                  network: netem.Impairment | None = None) -> str:
    """The base scenario as TOML, renamed, with the clients' -rbcharacter set to heroes in order,
    and through the internet relay with network's settings (default: the base scenario's)."""
    lines = [f"name = {_toml_str(name)}", f"step = {base.step}", f"smoke = {str(base.smoke).lower()}",
             f"time_limit_s = {base.time_limit_s}", f"pass_when = {_toml_str(base.pass_when)}",
             f"required_passes = {base.required_passes}"]
    if base.expect_map:
        lines.append(f"expect_map = {_toml_str(base.expect_map)}")
    lines.append(f"expect_combat = {str(base.expect_combat).lower()}")
    network = network or base.network
    if network:
        lines += ["", "[network]"] + [f"{k} = {v}" for k, v in asdict(network).items()]
    want = iter(heroes)
    for p in base.processes:
        args = list(p.args)
        if p.role == "client":
            hero = next(want, None)
            if hero is not None:
                args = [a for a in args if not a.startswith("-rbcharacter=")] + [f"-rbcharacter={hero}"]
        lines += ["", "[[process]]", f"name = {_toml_str(p.name)}", f"role = {_toml_str(p.role)}",
                  "args = [" + ", ".join(_toml_str(a) for a in args) + "]"]
    return "\n".join(lines) + "\n"


def _read_json(p: Path):
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def loaded_heroes(run_dir: Path) -> dict[str, str]:
    """Each client's last reported pawn_hero, from the run's timeline."""
    out: dict[str, str] = {}
    try:
        lines = (run_dir / "timeline.jsonl").read_text(encoding="utf-8").splitlines()
    except OSError:
        return out
    for line in lines:
        try:
            s = json.loads(line)
        except ValueError:
            continue
        st = s.get("state") if isinstance(s, dict) else None
        if isinstance(st, dict) and st.get("pawn_hero"):
            out[s.get("name", "?")] = str(st["pawn_hero"])
    return out


def combat_counts(run_dir: Path) -> dict[str, dict]:
    """Each client's kills and shots from the run's combat.json."""
    stats = _read_json(run_dir / "combat.json")
    if not isinstance(stats, dict):
        return {}
    return {n: {"kills": s.get("kills", 0), "shots": s.get("shots", 0)}
            for n, s in stats.items() if isinstance(s, dict)}


def row_of(item: dict, r: run.RunResult) -> dict:
    return {**item, "outcome": r.outcome.kind, "detail": r.outcome.detail, "signature": r.signature,
            "loaded": loaded_heroes(r.run_dir), "combat": combat_counts(r.run_dir),
            "run_id": r.run_id, "run_dir": str(r.run_dir), "elapsed_s": r.elapsed_s,
            "finished": datetime.now().isoformat(timespec="seconds")}


# Results files


def load_results(state_dir: Path, resume: bool, stem: str = "matrix") -> dict:
    """matrix.json's rows when resuming; otherwise a fresh file, keeping an old one aside as
    matrix-<time>.json so hours of results are never lost to a forgotten --resume."""
    f = state_dir / f"{stem}.json"
    if not f.exists():
        return {"started": datetime.now().isoformat(timespec="seconds"), "rows": []}
    old = _read_json(f)
    if resume and isinstance(old, dict) and isinstance(old.get("rows"), list):
        return old
    if resume:
        print(f"note: {f} is unreadable; starting over and keeping it aside")
    f.replace(f.with_name(f"{stem}-{datetime.fromtimestamp(f.stat().st_mtime):%Y%m%d-%H%M%S}.json"))
    return {"started": datetime.now().isoformat(timespec="seconds"), "rows": []}


def _cell(v) -> str:
    return str(v).replace("|", "/").replace("\n", " ")


def render_md(results: dict) -> str:
    rows = results.get("rows", [])
    n_pass = sum(r["outcome"] == "pass" for r in rows)
    lines = ["# Hero matrix", "",
             f"Co-op story mission ({BASE_SCENARIO}) with two players per run"
             + (f", through the \"{results['network']}\" internet relay" if results.get("network") else "")
             + f". Started {results.get('started', '?')}.", "",
             f"{n_pass} pass, {len(rows) - n_pass} fail, of {len(rows)} runs so far.", ""]
    heroes: dict[str, list[str]] = {}
    for r in rows:
        for h in (r["c1"], r["c2"]):
            heroes.setdefault(h, []).append(r["outcome"])
    failing = sorted(h for h, oc in heroes.items() if any(o != "pass" for o in oc))
    if failing:
        lines += ["Heroes in a failed run: " + ", ".join(failing), ""]
    lines += ["| Round | c1 | c2 | Outcome | Signature | Loaded (c1 / c2) | c1 kills/shots | "
              "c2 kills/shots | Run folder |",
              "|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        ld, cb = r.get("loaded", {}), r.get("combat", {})
        ks = {n: f"{cb[n].get('kills', 0)}/{cb[n].get('shots', 0)}" if n in cb else "-"
              for n in ("c1", "c2")}
        lines.append("| " + " | ".join(_cell(x) for x in (
            r.get("round", ""), r["c1"], r["c2"], r["outcome"], r.get("signature") or "",
            f"{ld.get('c1', '?')} / {ld.get('c2', '?')}", ks["c1"], ks["c2"],
            r.get("run_id", ""))) + " |")
    return "\n".join(lines) + "\n"


def save_results(state_dir: Path, results: dict, stem: str = "matrix") -> None:
    state_dir.mkdir(parents=True, exist_ok=True)
    atomic_write(state_dir / f"{stem}.json", json.dumps(results, indent=2))
    atomic_write(state_dir / f"{stem}.md", render_md(results))


def results_stem(network: str | None) -> str:
    """Relay sweeps keep their own results file, so they never set a direct sweep's aside."""
    return f"matrix-net-{network}" if network else "matrix"


# The sweep


def _run_pair(d, base: scenario.Scenario, item: dict, network: str | None = None) -> run.RunResult:
    """Write the pair's scenario to a temporary file (never into scenarios/, where the ladder would
    pick it up) and run it. The run folder keeps its own copy."""
    with tempfile.TemporaryDirectory(prefix="bbmatrix-") as tmp:
        path = Path(tmp) / f"{item['name']}.toml"
        name = item["name"] + (f"-net-{network}" if network else "")
        imp = netem.PRESETS[network] if network else None
        path.write_text(scenario_text(base, name, [item["c1"], item["c2"]], imp), encoding="utf-8")
        return d.run(scenario.load(path))


def sweep(d, heroes: list[str], rounds: int, resume: bool, network: str | None = None) -> int:
    if d.play_alive() is not None:
        print("a play session is running; end it with `python -m debugloop.loop stop-play` first")
        return PLAY_SESSION
    try:
        base = scenario.find_scenario(BASE_SCENARIO)
        d.preconditions(d.runs_dir, len(base.processes))
    except (scenario.ScenarioError, run.HarnessError) as e:
        print(f"HARNESS ERROR: {e}")
        return HARNESS
    stem = results_stem(network)
    results = load_results(d.state_dir, resume, stem)
    results["heroes"], results["rounds"], results["network"] = heroes, rounds, network
    items = plan(heroes, rounds)
    done = {r["name"] for r in results["rows"]}
    todo = [it for it in items if it["name"] not in done]
    print(f"matrix: {len(items)} pairs over {rounds} round(s), {len(items) - len(todo)} already done")
    save_results(d.state_dir, results, stem)
    if todo:
        b = d.build()   # once for the whole sweep
        if not b.ok:
            print("BUILD FAILED\n" + b.output[-3000:])
            return HARNESS
        try:
            d.deploy()
        except deploy.DeployError as e:
            print(f"HARNESS ERROR: deploy failed: {e}")
            return HARNESS
    errors = 0
    for i, item in enumerate(todo, 1):
        if d.play_alive() is not None:
            print("MATRIX STOPPED: a play session started; rerun with --resume when it ends")
            return PLAY_SESSION
        print(f"[{i}/{len(todo)}] round {item['round']}: c1 {item['c1']}, c2 {item['c2']}", flush=True)
        try:
            r = _run_pair(d, base, item, network)
            if r.outcome.kind != "pass" and not r.signature:
                raise run.HarnessError(f"run {r.run_id} ended '{r.outcome.kind}' with no signature")
        except Exception as e:   # a harness problem, not a result: never recorded, so --resume retries it
            if not isinstance(e, (run.HarnessError, scenario.ScenarioError, OSError)):
                traceback.print_exc(file=sys.stdout)
            errors += 1
            print(f"HARNESS ERROR ({errors}/{MAX_HARNESS_ERRORS}): {e}")
            if errors >= MAX_HARNESS_ERRORS:
                print(f"MATRIX STOPPED: {MAX_HARNESS_ERRORS} harness errors in a row; "
                      "fix the cause and rerun with --resume")
                return HARNESS
            continue
        errors = 0
        results["rows"].append(row_of(item, r))
        save_results(d.state_dir, results, stem)
        print(f"  {r.outcome.kind.upper()} {r.signature or ''} {r.outcome.detail}".rstrip())
    names = {it["name"] for it in items}
    rows = [r for r in results["rows"] if r["name"] in names]
    n_pass, missing = sum(r["outcome"] == "pass" for r in rows), len(items) - len(rows)
    print(f"MATRIX DONE: {n_pass} pass, {len(rows) - n_pass} fail"
          + (f", {missing} not run (harness errors; rerun with --resume)" if missing else "")
          + f"  (table: {d.state_dir / (stem + '.md')})")
    if n_pass < len(rows):
        return FAILED
    return HARNESS if missing else OK   # a pair that never ran proves nothing


def parse_heroes(arg: str | None, known: list[str]) -> list[str]:
    """--heroes as given (matched to the known names without case), or every hero."""
    if not arg:
        return list(known)
    by_lower = {h.lower(): h for h in known}
    out = []
    for h in (x.strip() for x in arg.split(",")):
        if not h:
            continue
        if h.lower() not in by_lower:
            raise ValueError(f"unknown hero {h!r}; known: {', '.join(known)}")
        if by_lower[h.lower()] not in out:
            out.append(by_lower[h.lower()])
    if len(out) < 2:
        raise ValueError("the matrix needs at least two heroes")
    return out


def main(argv=None, deps=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m debugloop.matrix")
    ap.add_argument("--heroes", help="comma-separated hero names (default: every hero)")
    ap.add_argument("--rounds", type=int, default=2,
                    help="rounds; each plays every hero, later ones with new partners and c1/c2 swapped")
    ap.add_argument("--resume", action="store_true", help="skip pairs already in the results file")
    ap.add_argument("--network", choices=sorted(netem.PRESETS),
                    help="play every pair through the internet relay with this preset "
                         "(results in state/matrix-net-<preset>.json/.md)")
    a = ap.parse_args(argv)
    try:
        heroes = parse_heroes(a.heroes, all_heroes())
    except (OSError, ValueError) as e:
        print(f"HARNESS ERROR: {e}")
        return HARNESS
    if a.rounds < 1:
        print("HARNESS ERROR: --rounds must be 1 or more")
        return HARNESS
    return sweep(deps or loop.Deps(), heroes, a.rounds, a.resume, a.network)


if __name__ == "__main__":
    sys.exit(main())
