#!/usr/bin/env python3
"""Run pgass over the curated cases in perf/ and report what each one cost.

Every case is one self-contained program, with a header saying how long it is
allowed to take and what it should answer:

    % difficulty: medium
    % expect: SAT
    % cost: 39

A case fails if it answers something other than what its header says, or if it
runs past the budget its difficulty carries. Easy and medium run by default,
which takes about a minute and a half.

    perf.py                     easy and medium
    perf.py --difficulty all    every case
    perf.py --filter still      the cases whose name contains 'still'
    perf.py --save              record these times as the baseline to compare to

The baseline lives in perf/.times.json and is not committed. Wall-clock seconds
do not carry between machines, but the change against your own last run is what
catches a case sliding from 0.4s to 7s while still fitting inside its budget.
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PERF = os.path.join(REPO, "perf")
BASELINE = os.path.join(PERF, ".times.json")
DEFAULT_PGASS = os.path.join(REPO, "build", "pgass")

# The exit codes of src/exit_code.h, as bench.py reads them.
SATISFIABLE = {10, 30, 62}
UNSATISFIABLE = {20}

# What each difficulty is allowed to take. The budgets sit well above what the
# cases measure, so that a slow machine does not fail the suite. A real
# regression shows up in the change against the baseline long before it reaches
# one of these.
BUDGETS = {"easy": 10, "medium": 60, "hard": 180}
DEFAULT_DIFFICULTIES = ["easy", "medium"]

# A time is only worth flagging when it grew by both of these. Either alone
# fires constantly: the ratio on cases that take a tenth of a second, and the
# difference on the slowest cases.
SLOWER_RATIO = 1.5
SLOWER_SECONDS = 1.0


def read_header(path):
    """The settings in a case's leading comment block."""
    header = {}
    with open(path) as f:
        for line in f:
            if not line.startswith("%"):
                break
            body = line[1:].strip()
            if ":" not in body:
                continue
            key, value = body.split(":", 1)
            key = key.strip().lower()
            if key in ("difficulty", "expect", "cost"):
                header[key] = value.strip()
    return header


def load_cases(pattern, difficulties):
    cases = []
    for path in sorted(glob.glob(os.path.join(PERF, "*.lp"))):
        name = os.path.basename(path)[: -len(".lp")]
        if pattern and pattern not in name:
            continue
        header = read_header(path)
        difficulty = header.get("difficulty")
        if difficulty not in BUDGETS:
            sys.exit(f"perf: {name}.lp has no difficulty the suite knows")
        if "expect" not in header:
            sys.exit(f"perf: {name}.lp does not say what to expect")
        if difficulty not in difficulties:
            continue
        cases.append((name, path, header))
    return cases


def status_of(code):
    if code in SATISFIABLE:
        return "SAT"
    if code in UNSATISFIABLE:
        return "UNSAT"
    return f"EXIT{code}"


def run_case(pgass, path, budget):
    started = time.time()
    try:
        done = subprocess.run([pgass, "--models=1", path],
                              capture_output=True, text=True, timeout=budget)
    except subprocess.TimeoutExpired:
        return {"status": "TIMEOUT", "seconds": float(budget)}

    cost = None
    for line in done.stdout.splitlines():
        if line.startswith("Cost:"):
            cost = " ".join(line[len("Cost:"):].split())
    return {"status": status_of(done.returncode),
            "seconds": round(time.time() - started, 2),
            "cost": cost,
            "stderr": (done.stderr.strip().splitlines() or [""])[0]}


def verdict(header, result, budget):
    """Why this case failed, or None if it passed."""
    if result["status"] == "TIMEOUT":
        return f"over budget ({budget}s)"
    if result["status"] != header["expect"]:
        return f"answered {result['status']}, header says {header['expect']}"
    if header.get("cost") and result.get("cost") != header["cost"]:
        return f"cost {result.get('cost') or 'none'}, header says {header['cost']}"
    return None


def delta_of(name, seconds, baseline):
    """How this time compares to the baseline, as text and whether it regressed."""
    was = baseline.get(name)
    if was is None:
        return "", False
    change = seconds - was
    slower = change >= SLOWER_SECONDS and seconds >= was * SLOWER_RATIO
    return f"{change:+.2f}", slower


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pgass", default=DEFAULT_PGASS)
    parser.add_argument("--difficulty", default=",".join(DEFAULT_DIFFICULTIES),
                        help="easy, medium, hard or all, comma separated")
    parser.add_argument("--filter", help="only cases whose name contains this")
    parser.add_argument("--save", action="store_true",
                        help="write these times to perf/.times.json")
    args = parser.parse_args()

    if not os.path.exists(args.pgass):
        sys.exit(f"perf: no pgass at {args.pgass}. Run 'make' first.")

    if args.difficulty == "all":
        difficulties = list(BUDGETS)
    else:
        difficulties = [d.strip() for d in args.difficulty.split(",")]
        unknown = [d for d in difficulties if d not in BUDGETS]
        if unknown:
            sys.exit(f"perf: no such difficulty '{unknown[0]}'")

    cases = load_cases(args.filter, difficulties)
    if not cases:
        sys.exit("perf: nothing to run")

    baseline = {}
    if os.path.exists(BASELINE):
        with open(BASELINE) as f:
            baseline = json.load(f)

    plural = "case" if len(cases) == 1 else "cases"
    print(f"{len(cases)} {plural}, {', '.join(difficulties)}\n")
    print(f"{'CASE':<32} {'RESULT':<7} {'SECONDS':>8} {'DELTA':>8}")

    # One at a time, so the seconds mean something.
    times, failed, slower = {}, [], []
    for name, path, header in cases:
        print(f"{name:<32} ", end="", flush=True)
        budget = BUDGETS[header["difficulty"]]
        result = run_case(args.pgass, path, budget)
        times[name] = result["seconds"]

        why = verdict(header, result, budget)
        change, regressed = delta_of(name, result["seconds"], baseline)
        row = f"{result['status']:<7} {result['seconds']:>7.2f}s {change:>8}"
        if why:
            failed.append((name, why))
            row += f"  FAILED: {why}"
        elif regressed:
            slower.append(name)
            row += "  SLOWER"
        print(row)

    total = sum(times.values())
    print(f"\n{len(cases)} {plural} in {total:.1f}s")
    if slower:
        print(f"{len(slower)} slower than the baseline: {', '.join(slower)}")
    for name, why in failed:
        print(f"FAILED {name}: {why}")

    if args.save:
        with open(BASELINE, "w") as f:
            json.dump(times, f, indent=2, sort_keys=True)
        print(f"Saved {len(times)} times to {os.path.relpath(BASELINE, REPO)}")

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
