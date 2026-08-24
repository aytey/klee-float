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

### It is Bitwuzla's abstraction module

Running the *same* queries through both solvers standalone settles it.
`--debug-bitwuzla-dump-queries` writes Bitwuzla's SMT-LIBv2 form of each query,
which unlike Z3's dump uses only standard operators — Z3 emits `fp.to_ieee_bv`,
its own extension, which neither other solver parses — so it is portable to STP
after rewriting SMT-LIB 2.6 `define-const` as `define-fun`.

Over 39 such queries from `sqrt_klee_bug`, all three solving all 39, three
repetitions:

| Configuration | Total |
| --- | --- |
| Bitwuzla, abstraction **on** (default) | 64.5 / 73.2 s |
| STP + MiniSat | 80.4 / 82.1 / 80.7 s |
| Bitwuzla, abstraction **off** | 90.2 / 103.0 s |

**With `--abstraction=false`, Bitwuzla is slower than STP.** Its entire lead
comes from its abstraction module, which replaces wide bit-vector operations
with abstractions refined on demand rather than bit-blasting them outright. The
blasted CNF for one query shows the same thing:

| Encoding | Variables | Clauses |
| --- | --- | --- |
| Bitwuzla, abstraction on | 45,010 | **142,659** |
| STP + MiniSat | 41,556 | 174,505 |
| Bitwuzla, abstraction off | 59,423 | 187,858 |

Solve time tracks clause count monotonically, so this is "bigger circuit", not
"same circuit, searched worse" — and **STP's own blasting is already better than
Bitwuzla's full blast**. What STP lacks is the abstraction layer on top.

The threshold makes the prediction testable: Bitwuzla abstracts bit-vector
operations of width ≥ 33 (`--abstraction-bv-size`). So it should help on wide
arithmetic and do nothing on narrow. Two minimal programs, one multiplying
32-bit values and one 64-bit:

| | STP | Bitwuzla on | Bitwuzla off |
| --- | --- | --- | --- |
| 32-bit multiply | **0.05s** | 0.06s | 0.06s |
| 64-bit multiply | 0.13s | **0.04s** | 0.11s |

Below the threshold abstraction is inert and STP is competitive; above it,
abstraction is worth 2.75× and puts Bitwuzla 3× ahead. That is exactly the
split seen across the suites: KLEE's integer queries are mostly 32-bit and
narrower, so STP wins them, while floating-point blasting produces the wide
multipliers inside `fp.mul`, `fp.div` and `fp.sqrt` on 24- and 53-bit
significands, so Bitwuzla wins those.

So the work needed to make STP win outright is in **lazy abstraction and
refinement of wide bit-vector arithmetic** — not KLEE's integration, not the
SAT solver, and not STP's blasting or its bitvector reasoning, all of which are
already competitive or better.

### STP already has that machinery, and enabling it does not close the gap

`lib/ToSat/BVAbstractionRefiner.cpp` is a CEGAR refiner over bit-blasted
bit-vector operations, reachable from the C API through `BV_TERM_ABSTRACTION`
(wide arithmetic, comparisons and ITE; off by default),
`BV_TERM_ABSTRACTION_MULT` (whether that covers `BVMULT`/`BVDIV`/`BVMOD`; on),
`BV_EQ_ABSTRACTION` (wide equalities; off), `BV_ABSTRACTION_WIDTH` (the floor,
**64** where Bitwuzla's is 33), `BV_EQ_REFINE_WIDTH` and
`BV_TERM_ABSTRACTION_ROUNDS` (the escalation budget, 32).

The width gap looks like the answer — a double's 53-bit significand multiplier
falls between STP's floor and Bitwuzla's — but measuring says otherwise. Over
the same 39 queries, three repetitions of each:

| Configuration | Median | Range | Solved |
| --- | --- | --- | --- |
| STP baseline | 84.55s | 82.47–87.92 | 39 |
| STP `--bv-term-abstraction --bv-abstraction-width 53` | 84.10s | 76.27–86.02 | 39 |
| Bitwuzla (abstraction on) | **63.03s** | 62.11–68.78 | 39 |

Enabling term abstraction at a safe width is **a wash** — 0.5% on the median,
better in two of three paired repetitions, ranges overlapping heavily. (A
single-run sweep suggested ~5%; that did not survive repetition.) Bitwuzla stays
1.34× ahead, and there its range is cleanly separated from both STP
configurations.

Lowering STP's floor to Bitwuzla's 33 is far worse than doing nothing:

| Configuration at width 33 | Total | Solved (30s cap) |
| --- | --- | --- |
| default (`rounds` 32) | 361.7s | 32 / 39 |
| `--bv-term-abstraction-rounds 4` | 319.5s | 33 / 39 |
| `--bv-term-abstraction-rounds 1` | 370.4s | 30 / 39 |
| `--bv-term-abstraction-mult 0` | **73.9s** | 39 / 39 |

Four to five times slower with queries left unsolved, and the escalation budget
does not rescue it — refining sooner or later is equally bad. Turning off just
the multiplier abstraction at the same width restores baseline performance, so
it is specifically `BVMULT`/`BVDIV`/`BVMOD` refinement that fails to converge on
these queries. Equality abstraction alone changes nothing either way.

### The lemma catalogue is not what makes Bitwuzla's abstraction work

Bitwuzla refines from a catalogue of algebraic lemma schemas — 19 for `BV_MUL`,
37 for `BV_UDIV`, 15 for `BV_UREM` (`LemmaKind` in
`src/solver/abstract/abstraction_lemmas.h`) — asserting facts true for *all*
operand values, such as `x * s = s << log2(x)` when `x` is a power of two. STP
has no equivalent: its only multiplier refinement is the value-blocking lemma
`(a ≠ aBits) ∨ (b ≠ bBits) ∨ result = expected`, which excludes one point of a
2^(2W) space and so cannot converge.

That looks like the whole explanation, and it is not.
`--abstraction-value-only` reduces Bitwuzla to exactly STP's strategy, and on
this corpus it costs nothing. Round-robin over the 39 queries — every
configuration run back to back on each query, so drift lands on all of them
equally — three passes:

| Configuration | pass 1 | pass 2 | pass 3 | total |
| --- | --- | --- | --- | --- |
| Bitwuzla, schemas (default) | 69.36 | 68.72 | 68.04 | 206.12s |
| Bitwuzla, `--abstraction-value-only` | 65.61 | 70.15 | 69.94 | **205.70s** |
| Bitwuzla, `--abstraction=false` | 91.42 | 99.10 | 93.40 | 283.92s |
| STP + MiniSat | 86.02 | 85.25 | 81.28 | 252.55s |

Schemas and value-only finish 0.2% apart. **The 28% that abstraction is worth
here comes from the framework — abstract wide operations, refine lazily,
escalate — not from the lemma catalogue.** Which sharpens the question rather
than answering it, because STP has that framework and enabling it makes things
worse. What differs is how the two spend it:

* **Budget.** Bitwuzla allows `bv_size / abstraction_value_limit` value lemmas,
  about 6 at 53 bits. STP allows a flat 32 whatever the width — five times more
  fruitless SAT calls before it gives up.
* **Escalation.** Bitwuzla emits a node-level lemma (`t = x op s`, optionally
  over the low bits only) that re-enters its ordinary rewriting and
  bit-blasting path. STP's `encodeMultiply`/`encodeDivMod` hand-emit a naive
  shift-add array straight into the SAT solver as gates, bypassing STP's AIG
  pipeline and ABC rewriting — so an abstraction STP gives up on ends up *worse
  encoded* than one it never abstracted, the likeliest reason `rounds=1`
  measured worse than baseline.

So the work item is narrower than "implement abstraction": the mechanism is
there, and what it needs is a cheaper way to spend it and to leave it — a
smaller value budget, and an escalation that goes through the same encoder as
an unabstracted term. Algebraic lemma schemas are the more interesting piece of
engineering and may matter on harder multiplier queries, but on this corpus
they are worth nothing over value-blocking, so they are not where to start.

All of these figures are one benchmark's 39 queries, so anything that looks
promising should be checked against the wider set before it is believed.

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

## STP's bit-vector abstraction, on this suite

STP can replace a wide bit-vector operation by free result bits and pin them
lazily by CEGAR. Four changes to that mechanism landed in STP
(`aytey_20260823_cegar_lemmas`), and on a corpus of 39 hard floating-point
queries dumped from `sqrt_klee_bug` they take it from **4.2x slower than not
abstracting to 1.4x faster**, and from 30/39 queries answered inside 30s to
39/39.

On fp-bench as a whole they change nothing, and turning the abstraction on
costs a quarter. Both statements are worth keeping, because the difference
between them is the point.

**KLEE never asked for the abstraction.** It is off in STP and
`--stp-bv-abstraction-width` (added for this) is zero by default, so every
earlier measurement in this file is of a code path that never ran, and the
new STP with it off is the same solver as the old one. That is the control
row below and it is what makes the rest readable.

Three interleaved passes, 76 benchmarks every configuration ran to a normal
halt in all three, solver time in seconds:

| configuration | pass 2 | pass 3 | pass 4 | median | vs. off | queries | ms/query |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Bitwuzla (CaDiCaL 2.1.2) | 873.6 | 846.1 | 867.4 | **867.4** | 0.98x | 5996 | 144.7 |
| STP + MiniSat, unchanged | 871.1 | 901.2 | 878.6 | **878.6** | 0.99x | 7646 | **114.9** |
| STP + MiniSat, new, abstraction off | 898.4 | 879.5 | 886.9 | **886.9** | 1.00x | 7602 | 116.7 |
| ... abstraction at 33 bits | 1082.9 | 1127.9 | 1130.4 | **1127.9** | 1.27x | 6884 | 163.8 |
| ... abstraction at 53 bits | 1102.2 | 1097.1 | 1128.4 | **1102.2** | 1.24x | 6944 | 158.7 |

So **STP and Bitwuzla finish level** on solver time, with STP 21% cheaper per
query and issuing 27% more of them — the same shape as the earlier
measurements in this file, now with the configurations interleaved rather
than run one after another. And the abstraction is a 24-27% loss, with
non-overlapping ranges.

### Where the loss is, and why the two workloads disagree

Not spread. Of the 234s the abstraction adds, **ten benchmarks account for
all of it**, and the top of that list is `sqr_double` and `sqr_longdouble`:

| benchmark | off | at 33 bits | queries (off / on) |
| --- | --- | --- | --- |
| `sqr_longdouble-flow` | 12.3s | 60.4s | 6 / 6 |
| `sqr_double-noflow` | 11.5s | 54.1s | 4 / 4 |
| `diction_style` | 8.3s | 42.3s | 34 / 34 |
| `sqr_longdouble-noflow` | 46.7s | 75.6s | 4 / 3 |
| `sqr_double-flow` | 4.5s | 22.2s | 6 / 6 |

The query counts are the same, so this is per-query cost and not more
exploration. These are **binary64 and x87 significand products — 53 and 64
bits** — and they are exactly what the abstraction is aimed at. On 27 of the
76 it is faster, saving 23s in total; on these five it loses 150s.

That is the disagreement with the 39-query corpus in one line: **the corpus is
entirely binary32**, so its abstracted multiplies are 24 to 33 bits, and it
cannot see the case that dominates here. It is a caution about the corpus and
not only about the abstraction — a 39-query benchmark drawn from one program
is a fast signal, not a workload.

The obvious suspect for the wide-operand loss is the blocking-lemma
allowance: at 53 bits a flat 32 of them enumerate 32 of 2^106 operand pairs
before the refinement gives up, which is 32 wasted solves.
`--stp-bv-abstraction-value-divisor` makes that allowance `width/divisor`
instead. It helps, and it does not rescue anything. One run of the five
benchmarks above:

| benchmark | off | at 33 | at 33, rate 8 |
| --- | --- | --- | --- |
| `sqr_double-noflow` | 6.8s | 56.8s | 34.5s |
| `sqr_longdouble-flow` | 11.9s | 63.1s | 39.3s |
| `sqr_double-flow` | 5.6s | 21.9s | 18.3s |
| `sqr_longdouble-noflow` | 49.0s | 74.4s | 84.7s |
| `diction_style` | 9.8s | 46.9s | 49.5s |

Three of five improve by a quarter to a third, two get worse, and the
abstraction stays three to six times worse than leaving it off in every one.
This is the evidence the binary32 corpus could not provide, and it is not
enough to move STP's default: the rate is a real effect on wide operands, but
inside a configuration nobody should be running on this suite anyway.

**The recommendation, therefore, is the default: leave it off.** The
abstraction is worth having on a workload of a few expensive queries over
narrow-significand floating point, which is what the corpus is and what this
suite is not.

## STP beats Bitwuzla once it stops solving incrementally

STP has two ways of deciding a query: a batch pipeline that simplifies the
whole formula and then bit-blasts it, and a persistent incremental driver
that keeps one solver alive across `vc_push`/`vc_pop`. It switches from the
first to the second automatically, and for an embedder that happens on the
**third query** -- the C API has no `set-logic`, so it cannot claim the
longer threshold STP's own sweep chose for pure bit-vector sessions.

KLEE pushes and pops per query, so every KLEE query from the third onwards
has been decided by the incremental driver. Nobody chose that, and it is
wrong here often enough to decide the whole comparison.

Per benchmark the two modes are three to six times apart **in both
directions**, with identical instruction and query counts -- the same
exploration either way:

| benchmark | incremental | batch |
| --- | --- | --- |
| `sparse_matrices_klee_bug` | 59.5s | **9.9s** |
| `libmatheval_sym_f` | 35.8s | **9.0s** |
| `sqr_longdouble-noflow` | 49.8s | **17.0s** |
| `vectors_klee` | 34.5s | **11.2s** |
| `sort_smallest_klee` | **18.1s** | 60.9s |
| `sort_smallest_klee_bug` | **26.6s** | 60.2s |

Neither extreme wins overall -- always-batch and always-incremental finish
within 2% of each other -- so what the threshold really selects is which
sessions get which. The number of queries does not predict the winner
(`libmatheval_sym_f` has 69 and wants batch; `sqr_longdouble-flow` has 6 and
wants the driver), and picking correctly per session would be worth about
another 13%.

`--stp-incremental-engage-at` exposes the threshold, and **32 is the
default**: the value STP's own sweep chose for bit-vector sessions and
withholds from embedders only because the C API cannot see a logic.

Three interleaved passes, 78 benchmarks every configuration ran to a normal
halt in all three, solver time:

| configuration | pass A | pass B | pass C | median | range |
| --- | --- | --- | --- | --- | --- |
| Bitwuzla | 1026.0 | 1029.6 | 988.8 | **1026.0s** | 989–1030 |
| STP, engage at 3 (before this) | 1006.8 | 1028.9 | 1002.3 | **1006.8s** | 1002–1029 |
| **STP, engage at 32** | 968.3 | 942.5 | 940.6 | **942.5s** | **941–968** |
| STP, engage at 48 | 952.0 | 971.9 | 930.6 | 952.0s | 931–972 |

**STP is 8.1% faster than Bitwuzla, and the ranges do not overlap.** Before
this the two were level. 48 is indistinguishable from 32; 32 has the better
median and is the value STP already believes in.

Nothing about what KLEE decides changes: 31 true positives, 8 unexpected and
3 missed under every configuration in every pass, and KLEE's own suite is
278 passed / 0 failed.

### What is left

STP still loses badly on a handful of queries, and they are not spread out --
the ten worst account for 127s of the 140s it gives away:

| benchmark | STP @32 | Bitwuzla |
| --- | --- | --- |
| `nan_longdouble` | 15.5s | **0.9s** |
| `nan_double` | 10.8s | **0.5s** |
| `sqr_longdouble-noflow` | 18.3s | **1.1s** |
| `sqr_longdouble-flow` | 25.6s | **4.6s** |
| `libmatheval_sym_f` | 32.1s | **10.3s** |

Two different problems are mixed together there. `libmatheval_sym_f` and
`sparse_matrices_klee_bug` are the threshold mis-assigning a session -- both
want batch and both have more than 32 queries. The `nan_*` and
`sqr_longdouble` rows are a real solver gap on fp80 and binary64: seventeen
to twenty times, on queries that are about NaN propagation and about
`sqrt(x*x)`.

The first of those is fixed below. The second is not, and is the thing to
pick up next.

## Measuring the choice instead of guessing it

A fixed ordinal has to guess which sessions want the driver, and it guesses
wrong often: `libmatheval_sym_f` has 69 queries and wants batch, while
`sqr_longdouble-flow` has 6 and wants the driver. Nothing about a session
that is visible before it runs predicts the answer.

So `--stp-adapt-incremental` (on by default) measures it. The first
`--stp-incremental-engage-at` queries run on the batch pipeline and their
mean cost is the baseline; the driver is then engaged and its own mean
compared against it. If the driver is more than `--stp-adapt-regret` times
slower per query it is abandoned for the rest of the session, after as few as
four engaged queries.

There is deliberately no matching decision to *keep* it. An early version
settled on the driver once it had run as many queries as the baseline had,
and on `sparse_matrices_klee_bug` that fired on the fourth engaged query and
kept a driver that then ran five times slower for the next nine hundred:
45.5s against 8.4s. Staying unsettled costs nothing, because the means keep
accumulating and the abandon test can still fire much later.

With the decision revisitable, the engagement point drops from 32 to **8** --
it is no longer a commitment, just where the measuring starts.

Three interleaved passes, 76 benchmarks every configuration ran to a normal
halt in all three:

| configuration | pass p | pass q | pass r | median | range |
| --- | --- | --- | --- | --- | --- |
| STP, engage at 3 (before any of this) | 898.5 | 890.6 | 910.2 | **898.5s** | 891–910 |
| Bitwuzla | 806.9 | 809.0 | 811.8 | **809.0s** | 807–812 |
| STP, fixed ordinal 32 | 792.5 | 791.2 | 797.2 | **792.5s** | 791–797 |
| STP, adaptive from 32 | 766.7 | 780.9 | 798.3 | 780.9s | 767–798 |
| **STP, adaptive from 8** | 743.7 | 741.8 | 742.3 | **742.3s** | **742–744** |

**STP is 8.3% faster than Bitwuzla, and 17.4% faster than it was**, with
ranges that do not overlap either comparison.

It also finds more. Every adaptive run reports **32 true positives and 2
missed** against Bitwuzla's 31 and 3 -- one more real, specified bug, in all
three passes, because it gets further inside the same budget. The extra
unexpected report is the `atof` true positive documented above.

KLEE's own suite: 278 passed, 0 failed.
