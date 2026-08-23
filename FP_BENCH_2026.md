# Running the fp-bench experiments in 2026

This records how to rebuild the floating-point benchmark suite this branch was
evaluated on, and how to run it across solver backends — specifically to compare
the STP floating-point support added here against Z3.

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
scripts/build-2026.sh                          # KLEE + LLVM 3.4 + Z3 + STP
scripts/fp-bench-2026/fetch-and-build.sh       # the benchmark suite
scripts/fp-bench-2026/run-all.sh               # 86 benchmarks x 5 solvers
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
  *on*, but `CoreSolver.cpp` only passes it to `STPSolver` — Z3 always runs
  in-process. Left at the default, STP would fork a process per query and Z3
  would not, which is not a like-for-like comparison.
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

## Reading the results

**Under a fixed time budget a faster solver does not finish sooner — it does
more work.** Totals of solver time are therefore not a like-for-like comparison,
because the configurations do not execute the same paths or issue the same
queries. `aggregate.py` reports three views for this reason: cost per query
(the fair aggregate), totals restricted to benchmarks every configuration ran to
a normal halt, and bugs found versus each benchmark's specification.

Results on this machine (86 benchmarks, 60s exploration budget, 8-way parallel):

| Solver | Solver time | Queries | **ms/query** | Instructions | Killed |
| --- | --- | --- | --- | --- | --- |
| **STP master** | 1250s | 8176 | **153** | 1,531,880 | 7 |
| Z3 4.5.0 | 1927s | 5276 | 365 | 1,360,846 | 6 |
| Z3 4.15.0 | 2479s | 5934 | 418 | 1,175,596 | 6 |
| Z3 5.0 | 2337s | 6170 | 379 | 1,191,061 | 7 |
| Z3 5.1 | 2193s | 6132 | 358 | 1,164,937 | 8 |

STP issued 55% more queries and covered more instructions in 35% less solver
time — 2.3–2.7× cheaper per query. Restricted to the 72 benchmarks every
configuration halted on, STP is 1.66× faster than Z3 4.5.0 and 1.84–2.10× faster
than the 5.x builds. Consistently with the KLEE test suite, the newer Z3 builds
are *slower* here than the 4.5.0 this branch pins.

Bug-finding agrees across all five: 31 true positives and 3 missed, with seven
further errors reported by *every* configuration — solver-independent, so
attributable to this 2026 environment rather than any backend.

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
issued **685 queries in 60s where Z3 5.0 and 5.1 managed 3**. The lesson for
anyone re-running this is that an unexpected error against a spec is worth
replaying natively before it is believed — in either direction.
