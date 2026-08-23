# Running the fp-bench experiments in 2026

This records how to rebuild the floating-point benchmark suite this branch was
evaluated on, and how to run it across solver backends — specifically to compare
the STP and Bitwuzla floating-point support added here against Z3.

The suite is the one from:

> **Floating-Point Symbolic Execution: A Case Study in N-Version Programming**
> Daniel Liew, Daniel Schemmel, Cristian Cadar, Alastair F. Donaldson,
> Rafael Zähl, Klaus Wehrle. ASE 2017.

Note what this is *not*: the paper's headline result is an N-version comparison
between the Imperial and Aachen KLEE extensions, and only the Imperial tool
lives in this repository. What follows runs *this* KLEE over the paper's
benchmarks under different solvers. It reuses the paper's benchmark suite; it
does not reproduce the paper's experiment.

## Quick start

```sh
scripts/build-2026.sh                          # KLEE + LLVM 3.4 + Z3 + STP + Bitwuzla
scripts/fp-bench-2026/fetch-and-build.sh       # the benchmark suite
scripts/fp-bench-2026/run-all.sh               # 86 benchmarks x 6 solvers
```

Everything lands in `$ROOT/fp-bench-2026` (override with `FP_BENCH_ROOT`);
nothing is installed system-wide.

## The benchmark suite

Three repositories, pinned to the commits `scripts/container.Makefile` uses —
i.e. what the paper's own container built:

| Repository | Commit | Paper ref |
| --- | --- | --- |
| `delcypher/fp-bench` | `440b9a24` | [28] (as `delcypher/symex-fp-bench`) |
| `delcypher/fp-benchmarks-imperial` | `f2c7c17d` | [30] |
| `delcypher/fp-benchmarks-aachen` | `265b1195` | [29] (upstream is `danielschemmel/…`; this is a fork) |

The build produces **138** benchmarks, of which the **86** tagged `issta_2017`
are the study set — the category is named for an earlier submission target, not
ASE. That 86 matches the paper exactly, including the per-team splits:

| | Paper | This build |
| --- | --- | --- |
| Aachen synthetic | 28 (17 correct / 11 incorrect) | 28 (17 / 11) |
| Aachen real-world | 15 (13 / 2) | 15 (13 / 2) |
| Imperial synthetic | 28 (15 / 13) | 28 (15 / 13) |
| Imperial real-world (GSL) | 15 (7 / 8) | 15 (7 / 8) |
| **Total** | **86** (52 / 34) | **86** (52 / 34) |

A caution when counting these yourself: a `spec.yml` declares one benchmark
*per variant*, but a spec with no `variants:` key declares a single benchmark
directly. `imperial/synthetic/paranoia` is one of those, so counting only
`variants:` blocks silently loses it. Use fp-bench's own `svcb` loader
(`svcb/tools/category-count.py`) rather than parsing the YAML by hand.

## What breaks on a 2026 system

Two problems, both in the vendored third-party code rather than the benchmarks,
and both handled by `fetch-and-build.sh`:

**Removed glibc headers.** libmatheval 1.1.11 includes `<libio.h>`,
`<_G_config.h>` and `<xlocale.h>`, all of which glibc has since deleted — their
contents moved into `<stdio.h>` and `<locale.h>`. `scripts/fp-bench-2026/compat/`
provides replacements, injected through `CPATH` so the benchmark sources are
not touched.

**`lib` vs `lib64`.** GSL and GMP are built as external projects and install to
`lib64/` on a distribution that uses it, while fp-bench looks for
`lib/libgsl.a` and `lib/libgmp.a`. The build script symlinks `lib -> lib64`
after the externals exist and resumes; it cannot do this up front because the
directories are not there until they have been built.

Pleasingly, fp-bench itself already handles one 2026 hazard: it detects Clang
3.4 and disables optimisation for `ld-unnormal`, which is exactly the
miscompilation the paper reports having hit.

## How the benchmarks are run

`run-one.sh` runs one benchmark under one solver. The options are chosen so that
the only thing differing between configurations is the solver:

* **`--use-forked-solver=false`.** This is the important one. It defaults to
  *on*, but `CoreSolver.cpp` only passes it to `STPSolver` — Z3 and Bitwuzla
  always run in-process. Left at the default, STP would fork a process per query
  and the others would not, which is not a like-for-like comparison. It is not a
  small effect: on the KLEE test suite STP spends 9.3s of solver time with
  forking and 7.3s without.
* **`--libc=uclibc`.** Without a libc these benchmarks cannot resolve `stdout`
  or `fprintf` and barely execute — `polyroots` runs 419 instructions without it
  and 5773 with it. `--posix-runtime` is not usable: their `main()` takes no
  arguments, and KLEE refuses.
* **`--max-time=60`, plus an outer `timeout -s KILL 150`.** KLEE's `--max-time`
  cannot fire while a single solver query is still running — the problem the
  paper's "dynamic solver timeout" was built to address — so a long query
  overruns it badly. The outer kill bounds that, identically for every solver.
  KLEE writes `run.stats` incrementally, so a killed run still reports the
  progress it made.

Z3 versions are swapped in at run time via `LD_LIBRARY_PATH`, pointing a
`libz3.so` symlink at each build. This works because KLEE only uses Z3's C API,
and all versions tested pass the KLEE test suite identically. It is not a
rebuild, so treat it as a strong indication rather than a controlled experiment.
Point `Z3_415_LIB` / `Z3_50_LIB` / `Z3_51_LIB` elsewhere to test other builds.
STP and Bitwuzla are whatever KLEE was linked against, since KLEE has a distinct
backend for each rather than a swappable library.

## Reading the results

**Under a fixed time budget a faster solver does not finish sooner — it does
more work.** Totals of solver time are therefore not a like-for-like comparison,
because the configurations do not execute the same paths or issue the same
queries. `aggregate.py` reports three views for this reason: cost per query
(the fair aggregate), totals restricted to benchmarks every configuration ran to
a normal halt, and bugs found versus each benchmark's specification.

Results on this machine (86 benchmarks, 60s exploration budget, 8-way parallel):

| Solver | Solver time | Queries | ms/query | Instructions | Killed |
| --- | --- | --- | --- | --- | --- |
| **Bitwuzla 0.9.1-dev** | **1098s** | 6598 | 166 | 1,471,420 | 7 |
| STP master | 1250s | 8176 | **153** | 1,531,880 | 7 |
| Z3 4.5.0 | 1927s | 5276 | 365 | 1,360,846 | 6 |
| Z3 4.15.0 | 2479s | 5934 | 418 | 1,175,596 | 6 |
| Z3 5.0 | 2337s | 6170 | 379 | 1,191,061 | 7 |
| Z3 5.1 | 2193s | 6132 | 358 | 1,164,937 | 8 |

Restricted to the 72 benchmarks every configuration halted on, Bitwuzla totals
710s, STP 786s, Z3 4.5.0 1307s and the 5.x builds 1449–1473s.

**Do not read the totals as "Bitwuzla is faster than STP".** Under this design
the configurations do not execute the same work, and here they never do: of the
86 benchmarks, **zero** have identical instruction counts across the backends —
not even among the 59 that every backend finished well inside the budget. KLEE
concretises symbolic arguments to external calls and *constrains* them to the
value the solver picked, so a different model prunes the state space
differently, and the divergence compounds. On top of that, 33 of the 86 are
budget-bounded for at least one backend, where a faster solver simply walks
further into the program.

That is what the query counts reflect: STP issued 8176 queries to Bitwuzla's
6598 because it explored further (1,531,880 instructions against 1,471,420),
not because it needs more queries per unit of work. See "Comparing solvers
fairly" below.

Bug-finding agrees across all six: 31 true positives and 3 missed, with seven
further errors reported by *every* configuration — solver-independent, so
attributable to this 2026 environment rather than any backend. STP additionally
reports `atof`, discussed below.

### Which SAT backend for STP?

STP bundles no SAT solver and picks a default by precedence — CaDiCaL, then
CryptoMiniSat, then Riss, then MiniSat (`UserDefinedFlags.h`) — so a build with
several enabled does not measure the one you think. Each configuration below is
a build with exactly one enabled, swapped in at run time by pointing
`libstp.so.2.4` at it.

Per-query cost, on both workloads (the KLEE test suite figures are over the
tests every configuration explored identically, median of three runs; the
fp-bench figures are single runs under the 60s budget — see the warning below
about reading that column):

| STP SAT backend | test suite ms/query | fp-bench ms/query (whole set) |
| --- | --- | --- |
| **MiniSat** (2008) | 5.07 | 149.1 |
| CryptoMiniSat 5.14 | **4.81** | 169.5 |
| CaDiCaL 3.0.1 | 5.44 | 162.2 |
| CaDiCaL 2.2.1 | 5.80 | 190.7 |
| *(Bitwuzla, CaDiCaL 2.1.2 internally)* | *4.79* | *166.4* |

**CaDiCaL is not an improvement for STP on this workload** — it is slower than
MiniSat on both, and 2.2.1 is consistently the worst of the four. That is worth
knowing because CaDiCaL is the modern default choice and is what Bitwuzla uses
internally, so it is the natural thing to reach for. MiniSat and CryptoMiniSat
trade places between the two workloads, so neither is clearly ahead.

The practical consequence is that the STP results reported here, built against
MiniSat, are not handicapped by that choice.

### Comparing solvers fairly

**A whole-set `ms/query` figure over fp-bench is not a per-query cost, and it
inverts the answer.** Of the 86 benchmarks, 20 are budget-bounded for one
backend or the other. On those, solver time is pinned near the budget whatever
the solver, so `ms/query` degenerates into the reciprocal of throughput — and
because they are also the *hardest* benchmarks, they dominate the aggregate.
Splitting them out (three repetitions each, medians):

| | STP+MiniSat | Bitwuzla | queries (STP / Bwz) |
| --- | --- | --- | --- |
| 66 never budget-bounded | 130.2 ms/query | **82.8 ms/query** | 2037 / 2033 |
| 20 budget-bounded | **157.2 ms/query** | 203.7 ms/query | 6102 / 4565 |
| whole set | **150.5 ms/query** | 166.4 ms/query | — |

On the 66 that run to completion the query counts match to 0.2%, so that row is
a genuine like-for-like per-query cost, and **Bitwuzla is about 1.6× faster
than STP+MiniSat**. On the 20 hardest, both spend the budget and STP gets
through 34% more queries. The whole-set row favours STP only because the second
group dominates it.

Three query difficulty regimes, then, and they do not agree:

| Workload | Typical cost | Result |
| --- | --- | --- |
| KLEE test suite | ~5 ms/query | indistinguishable (see below) |
| fp-bench, completing | ~100 ms/query | Bitwuzla ~1.6× faster |
| fp-bench, budget-bound | hardest | STP ~34% more queries in the same time |

### Where STP's remaining gap actually is

Two candidate explanations were measured and ruled out, which leaves one.

**Not KLEE's integration.** `STPBuilder` clears its construct cache after every
top-level `construct()`, whereas the Z3 and Bitwuzla builders share terms across
a whole query — so STP rebuilt subexpressions shared between a query's
constraints once per constraint, doing 231 constructions per query against
Bitwuzla's 106. Giving `STPBuilder` the same `autoClearConstructCache` flag
brought that to 105, exactly in line. Solver time did not improve
(−3.0%, inside the noise band), so the redundant construction was real but
cheap. The change was reverted; it is recorded here so the experiment is not
repeated.

**Not the SAT backend.** See above — CaDiCaL and CryptoMiniSat were both
measured and neither beats MiniSat for STP here.

**It is the floating-point theory.** Splitting the identically-explored test
suite by whether a test uses floating point (9 repetitions each, medians):

| Subset | STP+MiniSat | Bitwuzla | ratio | queries |
| --- | --- | --- | --- | --- |
| floating point (66 tests) | 2.56s | **2.12s** | 1.21× | 524 / 526 |
| bitvector only (172 tests) | **4.59s** | 5.26s | 0.87× | 1047 / 1043 |
| all (238 tests) | 7.13s | 7.36s | 0.97× | 1571 / 1569 |

**STP is already faster than Bitwuzla on bitvector-only queries** — faster in 73
of 81 run pairings — and slower only on floating-point ones, where it loses 80
of 81 pairings. The two effects roughly cancel over the whole suite, which is
why the aggregate looked like a tie.

So the work needed to make STP win outright is in its floating-point layer, not
in KLEE, not in the SAT solver, and not in bitvector reasoning. The gap also
widens with query difficulty — 1.21× on the test suite's small queries against
about 1.6× on the fp-bench benchmarks that run to completion — which points at
what reaches the bit-blaster (word-level rewriting and the size of the blasted
FP circuits) rather than at a constant overhead. STP's floating-point support is
also far younger than Bitwuzla's SymFPU integration, so there is likely room.

### How much of this is noise?

Enough to have misled an earlier version of this document, which claimed STP was
"about 7% ahead" of Bitwuzla on the test suite. It is not. Two independent
measurement sets of the *same* STP+MiniSat configuration differed by 7.0%, so
that gap was inside the noise floor.

Over nine repetitions each, on the 238 tests explored identically by both:

| Solver | Median | Mean | SD | Range |
| --- | --- | --- | --- | --- |
| STP+MiniSat | 7.13s | 7.15s | 0.46s (6.5%) | 6.58–8.20 |
| Bitwuzla | 7.36s | 7.35s | 0.42s (5.7%) | 6.88–8.18 |

A 3.1% median difference, STP faster in 64% of the 81 run pairings, ranges
fully overlapping: **not a real difference** at this query size. Both remain
roughly 4× faster than Z3 4.5.0, which is far outside the noise.

Because fp-bench exploration always diverges, the test suite is the better
instrument for *identical-work* comparison — 252 of its 260 tests explore
identically across Z3, STP and Bitwuzla, with query counts agreeing to within
0.4%. So there is no query-issuance inefficiency in any backend to tune away;
the only lever is cost per query, and which solver wins there depends on how
hard the queries are.

### The `atof` result

STP reports an assertion failure in Aachen's `atof` that no Z3 configuration
finds, and `atof` is specified `correct: true`. That reads like a false positive
from the new STP builder, which would be the worst possible outcome — so it is
worth recording why it is not one.

Replaying the generated test case natively, which is how the paper validated its
own results:

```sh
KTEST_FILE=…/stp/atof_default.x86_64/test000033.ktest \
LD_LIBRARY_PATH=$ROOT/klee-build/lib \
  …/build_O2/benchmarks/c/aachen/syn/atof_default.x86_64
```

fails the assertion for real. The test supplies bytes that, after the benchmark
overwrites `input[0]='0'` and `input[1]='.'`, spell `0.2e+1`; `atof` returns
**2.0**, so `assert(out < 1)` genuinely fails. The benchmark's specification
overlooks exponent notation.

STP found it purely by getting further within the budget: on that benchmark it
issued **685 queries in 60s where Z3 5.0 and 5.1 managed 3**. Bitwuzla does not
reach it either, for the same reason. The lesson for anyone re-running this is
that an unexpected error against a spec is worth replaying natively before it is
believed — in either direction.
