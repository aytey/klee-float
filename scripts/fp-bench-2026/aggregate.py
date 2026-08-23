#!/usr/bin/env python3
"""Summarise an fp-bench solver comparison produced by run-all.sh.

Reads results.psv plus each run's run.stats, and reports:

  * per-query solver cost -- the fair aggregate under a fixed time budget,
    where a faster solver does more work rather than finishing sooner;
  * totals restricted to benchmarks every configuration ran to a normal halt;
  * bugs found, and how those line up with each benchmark's specification.

Set FP_BENCH_OUT to point at a runs directory other than the default.
"""
import ast
import collections
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.abspath(os.path.join(HERE, "..", ".."))
ROOT = os.environ.get("KLEE_FLOAT_ROOT", os.path.dirname(SRC))
WORK = os.environ.get("FP_BENCH_ROOT", os.path.join(ROOT, "fp-bench-2026"))
OUT = os.environ.get("FP_BENCH_OUT", os.path.join(WORK, "runs"))
BUILD = os.path.join(WORK, "build_O2")


def run_stats(cfg, name):
    """Final (cumulative) row of a run's run.stats, or {} if it never wrote one."""
    try:
        lines = [l.strip() for l in open(os.path.join(OUT, cfg, name, "run.stats")) if l.strip()]
        return dict(zip(ast.literal_eval(lines[0]), ast.literal_eval(lines[-1])))
    except Exception:
        return {}


def expected_verdicts():
    """benchmark -> True if the spec expects it to be correct (no bug)."""
    try:
        import yaml
    except ImportError:
        return {}
    exp = {}
    for y in glob.glob(os.path.join(BUILD, "**", "*.x86_64.yml"), recursive=True):
        try:
            spec = yaml.safe_load(open(y))
        except Exception:
            continue
        tasks = spec.get("verification_tasks") or {}
        if tasks:
            exp[os.path.basename(y)[:-4]] = all(t.get("correct", True) for t in tasks.values())
    return exp


def main():
    results = os.path.join(OUT, "results.psv")
    if not os.path.exists(results):
        sys.exit("no results at %s -- run run-all.sh first" % results)

    rows = collections.defaultdict(dict)
    for line in open(results):
        parts = line.strip().split("|")
        if len(parts) != 6:
            continue
        cfg, name, rc, wall, errs, done = parts
        st = run_stats(cfg, name)
        rows[name][cfg] = dict(
            rc=int(rc), wall=float(wall), errs=int(errs), done=int(done),
            solver=st.get("SolverTime", 0.0), queries=st.get("NumQueries", 0),
            instr=st.get("Instructions", 0), covered=st.get("CoveredInstructions", 0))

    configs = sorted({c for v in rows.values() for c in v})
    complete = [n for n in rows if len(rows[n]) == len(configs)]
    print("benchmarks: %d, all %d configs: %d" % (len(rows), len(configs), len(complete)))
    if not complete:
        return

    def total(cfgname, field, names=None):
        return sum(rows[n][cfgname][field] for n in (names or complete))

    print("\n=== whole set (fixed budget: a faster solver does MORE work) ===")
    print("%-9s %10s %10s %9s %11s %10s %7s %6s" %
          ("config", "solverT", "wall", "queries", "ms/query", "instr", "killed", "errs"))
    for c in configs:
        q = total(c, "queries")
        st = total(c, "solver")
        killed = sum(1 for n in complete if rows[n][c]["rc"] != 0)
        print("%-9s %10.1f %10.1f %9d %11.1f %10d %7d %6d" %
              (c, st, total(c, "wall"), q, (1000 * st / q) if q else 0,
               total(c, "instr"), killed, total(c, "errs")))

    finished = [n for n in complete
                if all(rows[n][c]["rc"] == 0 and rows[n][c]["done"] > 0 for c in configs)]
    if finished:
        print("\n=== benchmarks every config ran to a normal halt (%d) ===" % len(finished))
        base = min(total(c, "solver", finished) for c in configs) or 1.0
        print("%-9s %10s %9s %9s %10s" % ("config", "solverT", "ratio", "queries", "instr"))
        for c in configs:
            st = total(c, "solver", finished)
            print("%-9s %10.2f %8.2fx %9d %10d" %
                  (c, st, st / base, total(c, "queries", finished), total(c, "instr", finished)))

    exp = expected_verdicts()
    if exp:
        print("\n=== bugs found vs specification ===")
        print("%-9s %15s %12s %12s" % ("config", "true positives", "unexpected", "missed"))
        for c in configs:
            tp = fp = missed = 0
            for n in complete:
                if n not in exp:
                    continue
                found = rows[n][c]["errs"] > 0
                if found and not exp[n]:
                    tp += 1
                elif found and exp[n]:
                    fp += 1
                elif not found and not exp[n]:
                    missed += 1
            print("%-9s %15d %12d %12d" % (c, tp, fp, missed))
        shared = set.intersection(*[
            {n for n in complete if exp.get(n, True) and rows[n][c]["errs"] > 0} for c in configs])
        if shared:
            print("\n  reported by every config (so not solver related): %d" % len(shared))
            for n in sorted(shared):
                print("    %s" % n)
        for c in configs:
            extra = {n for n in complete if exp.get(n, True) and rows[n][c]["errs"] > 0} - shared
            if extra:
                print("  only %s: %s" % (c, ", ".join(sorted(extra))))


if __name__ == "__main__":
    main()
