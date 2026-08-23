# Building klee-float in 2026

This branch is KLEE 1.3 plus symbolic floating point, written against a 2017
toolchain: **LLVM/Clang 3.4** and a **2016-era Z3**. No current distribution
ships either, and neither builds with a current GCC, so both have to be built
from source. This document records what is needed and, more usefully, *why* —
every workaround below is a concrete incompatibility between this code and a
2026 Linux system, not a matter of taste.

Verified on openSUSE Tumbleweed (glibc 2.43, GCC 11 default, CMake 4.1), where
the full system test suite passes: **278 passed, 0 failed**. Both solver
backends work on floating point — see "Floating point with STP" below.

## Quick start

```sh
scripts/build-2026.sh
```

Everything is built out-of-tree and nothing is installed system-wide:

```
$ROOT/klee-float    this checkout
$ROOT/deps          LLVM 3.4.2, Z3 4.5.0, STP (+ MiniSat, LibBF), klee-uclibc,
                    CMake 3.31, lit, wrapper scripts
$ROOT/klee-build    the KLEE build tree
```

`$ROOT` defaults to the parent of this checkout; override with
`KLEE_FLOAT_ROOT`. The script is idempotent — each dependency is skipped if it
is already built, so re-running it just reconfigures and rebuilds KLEE.

Run the tests with:

```sh
cd $ROOT/klee-build && PATH=$ROOT/deps/shim-bin:$PATH make systemtests
```

The `PATH` entry matters: some of KLEE's own helper scripts (`ktest-tool`,
`klee-stats`) still have `#!/usr/bin/env python`, and modern distributions have
no `python` binary. `deps/shim-bin/python` supplies one.

## Prerequisites

| Package | Why |
| --- | --- |
| `gcc7`, `gcc7-c++` | **Required.** LLVM 3.4 and Z3 4.5.0 do not compile with GCC 11+. GCC 7 is the newest GCC that still builds both. It also supplies the `crt*.o`, `libgcc` and libstdc++ headers that Clang 3.4 can use. |
| `python3` | LLVM 3.4's `configure`, klee-uclibc's `configure` and Z3's `mk_make.py` all want an interpreter; all three work under Python 3 via a `python` → `python3` shim. |
| `git`, `curl`, `make`, `patch` | Fetching, patching and building the dependencies. |
| ncurses and zlib development headers | LLVM, and klee-uclibc's `configure` probe. |
| a current GCC (the default one), `bison`, `flex`, Boost program_options | Building STP, which is C++17 and so cannot use GCC 7. |

## Dependency versions, and why those

| Dependency | Version | Reason |
| --- | --- | --- |
| LLVM + Clang | **3.4.2** (plus one patch, below) | `.travis.yml` and the `Dockerfile` pin LLVM 3.4; the source is full of `LLVM_VERSION_CODE < LLVM_VERSION(3,5)` guards. 3.4.2 is the last 3.4 point release, matching the `release_34` branch the original Docker image built from. |
| Z3 | **4.5.0** | `lib/Solver/Z3Solver.cpp` sets `rewriter.hi_fp_unspecified`, first available in 4.5.0. It is also late enough to have the full `Z3_mk_fpa_*` API and early enough to still define `Z3_TRUE`/`Z3_bool`, which later Z3 releases removed (`Z3Solver.cpp` uses `Z3_TRUE` directly). |
| klee-uclibc | `klee_uclibc_v1.0.0` | The branch named in `.travis.yml` for the LLVM 3.4 configurations. |
| CMake | **3.31.6** (a local binary tarball) | See below — CMake 4 refuses to configure this project at all. |
| lit | current, in a venv | The `utils/lit` in the LLVM 3.4 tree is Python 2 only. Modern lit runs this test suite fine. |
| STP | upstream master, pinned | For its floating-point theory — see below. Built with the system compiler, along with MiniSat (a SAT backend; STP bundles none, and this machine has neither CaDiCaL nor CryptoMiniSat) and LibBF (fetched by STP's own pinned, checksummed script). |

Only the X86 backend is built (`--enable-targets=host`); it is all KLEE needs.

## Floating point with STP and Bitwuzla

klee-float was written when STP had no floating-point theory, which is why its
floating-point support is Z3-only and why the benchmarks all run with
`--solver-backend=z3`. STP has one now (built on SymFPU) plus a C API for it, so
`lib/Solver/STPBuilder.cpp` implements the same translation against STP and
`--solver-backend=stp` works on floating-point programs.

`lib/Solver/Bitwuzla{Builder,Solver}.cpp` add a third FP-capable backend,
`--solver-backend=bitwuzla`. All three pass the whole test suite.

The translation deliberately mirrors `Z3Builder`, because the hard-won details
live there:

* Expressions are carried around as **bitvectors** and converted to STP's
  floating-point sort only where a floating-point operation needs one.
  `castToFloat()`/`castToBitVector()` do that on demand, keyed off
  `vc_getExpWidth()`, which answers 0 for anything that is not a float. Every
  operation that can be handed either sort — `eqExpr()`, `iteExpr()`, extraction,
  the shifts, the bitvector arithmetic and the comparisons — coerces its
  operands, because an `FAdd` result flowing into `x & 16` is ordinary in this
  codebase.
* **x87 fp80** is modelled as an IEEE format with a 15 bit exponent and a 64 bit
  significand, whose widths sum to 79 rather than 80 because x87's integer bit is
  explicit rather than hidden. Converting to and from it repacks the bit pattern
  and emits a *side constraint* pinning that explicit bit, so that a model comes
  back as a valid x87 fp80. `STPSolver` asserts those once the whole query is
  built and clears them afterwards, exactly as the Z3 solver does.

The API maps essentially one-to-one:

| KLEE / Z3 | STP |
| --- | --- |
| `Z3_mk_fpa_to_fp_bv` | `vc_fpToFPFromIEEEBV` |
| `Z3_mk_fpa_to_ieee_bv` | `vc_fpToIEEEBV` |
| `Z3_mk_fpa_to_fp_float` | `vc_fpToFPFromFP` |
| `Z3_mk_fpa_to_{u,s}bv` | `vc_fpTo{U,S}BVExpr` |
| `Z3_mk_fpa_to_fp_{un,}signed` | `vc_fpToFPFrom{Unsigned,Signed}BV` |
| `Z3_mk_fpa_{add,sub,mul,div,sqrt,abs}` | `vc_fp{Add,Sub,Mul,Div,Sqrt,Abs}Expr` |
| `Z3_mk_fpa_{eq,lt,leq,gt,geq}` | `vc_fp{Eq,Lt,Leq,Gt,Geq}Expr` |
| `Z3_mk_fpa_is_*` | `vc_fpIs*Expr` |
| `Z3_mk_fpa_round_*` | `vc_fpRoundingMode(VC_RM_*)` |

Z3 remains the default backend and nothing about the Z3 path changes — but note
that this took a fix. `CmdLineOptions.cpp` picked the default with an `#ifdef`
chain that tested `ENABLE_STP` first, so merely *building* with STP enabled
silently made STP the default for any invocation without `--solver-backend`.
Since this fork's floating-point work was developed and published against Z3,
the chain now prefers Z3 when it is available.

### What Bitwuzla needs that Z3 and STP did not

Bitwuzla implements the SMT-LIB floating-point theory faithfully, which means it
has **no float-to-bits operation**: `fp.to_ieee_bv` is a Z3 extension. So
`castToBitVector()` introduces a bitvector variable and constrains it, as a side
constraint, to reinterpret as the float. Two consequences:

* The variable is **cached per float term**. Casting the same float twice must
  not yield two variables that a model could give different bits.
* The constraint leaves **the bits of a NaN free**, because every NaN encoding
  reinterprets to the same NaN value. KLEE's own constant folding produces one
  specific encoding (`ConstantExpr::GetNaN()`, via `--single-repr-for-nan`), and
  Z3's `fp.to_ieee_bv` canonicalises to match it — so leaving it free yields
  models KLEE disagrees with, and `IndependentSolver`'s
  `assertCreatedPointEvaluatesToTrue` fires. `Floats/fneg.c` and the `fabs`
  tests catch it. The NaN case is therefore pinned to that same encoding.

Two smaller differences, both caught by the test suite:

* Bitwuzla **rejects a bitvector value that does not fit its sort**, where Z3
  truncates silently. `bvMinusOne()` asks for `~0` at every width, so the
  constant helpers narrow explicitly.
* KLEE overshifts an arithmetic right shift **to zero**, whereas `bvashr`
  saturates to the sign bit, so both shift paths guard for it.
  (`Solver/overshift-aright-by-constant.kquery`.)

Bitwuzla is not built by `scripts/build-2026.sh`: its own build fetches
dependencies and wants meson/ninja. Point `BITWUZLA_SRC` (or `BITWUZLA_LIB` and
`BITWUZLA_INCLUDE`) at an existing checkout; the build proceeds without it if
they are absent.

### Linking KLEE against STP

STP is C++17 and will not build with GCC 7, so it is built with the system
compiler while KLEE is still built with GCC 7. That is safe — KLEE talks to STP
only through `stp/c_interface.h`, so no C++ type crosses the boundary — but the
link needs one adjustment: GCC 7 resolves `-lstdc++` against its own, older,
copy, which lacks the `GLIBCXX_3.4.26`+ symbols a current `libstp.so` needs, and
the link fails even though the loader would resolve them from the system library
at run time. The build therefore points the link at the libstdc++ that will
actually be loaded:

```
-DCMAKE_CXX_STANDARD_LIBRARIES=/usr/lib64/libstdc++.so.6
```

### Benchmarks

`FP_BENCH_2026.md` covers rebuilding the fp-bench floating-point suite this
branch was evaluated on (the 86 benchmarks from the ASE 2017 paper) and running
it across solver backends, along with the scripts in `scripts/fp-bench-2026/`.

### Running the tests against STP or Bitwuzla

`make systemtests` exercises the default (Z3) backend. To run the same suite
against another:

```sh
cd $ROOT/klee-build && PATH=$ROOT/deps/shim-bin:$PATH $ROOT/deps/pyenv/bin/lit -v \
  --param klee_opts='--solver-backend=stp --use-forked-solver=false' \
  --param kleaver_opts=--solver-backend=stp test
```

`--use-forked-solver=false` matters for a fair comparison: it defaults to *on*,
but `CoreSolver.cpp` passes it only to `STPSolver`, so STP would fork a process
per query while Z3 and Bitwuzla run in-process.

All 67 `test/Floats` tests pass under each backend, as does the rest of the
suite. The one test that reports a failure that way,
`Solver/FastCexSolver.kquery`, is an artefact of the invocation rather than of
any solver: that test already passes `--solver-backend=dummy` itself, so
injecting a second `--solver-backend` makes the option appear twice and KLEE's
command line parser rejects it.

## What breaks on a modern system

### 1. CMake 4 will not configure this project

`CMakeLists.txt` does `cmake_policy(SET CMP0054 OLD)`. CMake 4.x removed the OLD
behaviour of every policy introduced before 3.5, and errors out rather than
warning. `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` does not help. The build script
therefore downloads a CMake 3.31 binary tarball and uses that.

LLVM 3.4's own CMake files have a related problem — they use `FindPythonInterp`,
which CMake 4 removed — which is why LLVM is built with its autoconf build
system instead.

### 2. The runtime build chokes on `MAKEFLAGS=""`

`runtime/CMakeLists.txt` runs the bitcode build as:

```cmake
COMMAND ${ENV_BINARY} MAKEFLAGS="" ${MAKE_BINARY} -f Makefile.cmake.bitcode all
```

Older CMake let the shell strip those quotes. Modern CMake escapes them, so
`make` is handed the literal two-character value `""` and dies with
`invalid option -- '"'`.

Rather than patch the source, the build passes `-DMAKE_BINARY=` pointing at
`deps/shim-bin/make-clean-flags`, a wrapper that clears `MAKEFLAGS` properly
before exec'ing the real `make`.

### 3. Clang 3.4 cannot find any modern GCC

Clang 3.4 predates the GCC layout of a current distribution. Left alone it
cannot link a native binary at all:

```
ld: cannot find crtbegin.o
ld: cannot find -lgcc
```

and in C++ mode it finds no standard headers whatsoever (`fatal error: 'cassert'
file not found`), because it does not recognise a current libstdc++'s version or
layout. `--gcc-toolchain=/usr` does not help: Clang 3.4 looks for
`/usr/lib/gcc/<triple>/<version>`, not openSUSE's `/usr/lib64/gcc/...`.

`deps/shim-bin/klee-clang` and `klee-clang++` (used as `LLVMCC` / `LLVMCXX`) add
`-B`/`-L` for GCC 7's runtime directory, and for C++ also `-I` for GCC 7's
libstdc++ headers, which Clang 3.4 can still parse.

One subtlety, which costs an afternoon if missed: the wrappers deliberately pass
`-print-*` invocations through **unmodified**. klee-uclibc's `Rules.mak` does
`-isystem $(shell $(CC) -print-file-name=include)` to find the compiler's own
header directory; under `-B` that answers GCC's include directory instead of
Clang's resource directory, and uClibc's `limits.h` then `#include_next`es into
glibc's, which fails on undefined `__GLIBC_USE()`.

Clang 3.4 does, pleasantly, cope with glibc 2.43's C headers: `bits/floatn.h`
gates `__HAVE_FLOAT128` on `__glibc_clang_prereq (3, 9)`, so old Clang never
sees the `_Float128` types it does not understand.

### 4. Clang 3.4's `__builtin_signbit` silently loses the sign of a NaN

This one changes results rather than breaking the build, so it is the important
one.

Clang 3.4 declares the builtin as `int(double)`:

```c
BUILTIN(__builtin_signbit, "id", "nc")
```

so Sema converts the argument first — promoting a `float`, demoting a
`long double`. glibc ≥ 2.32 defines `signbit(x)` as `__builtin_signbit (x)` for
Clang ≥ 3.3 (in 2017 that guard did not exist and Clang 3.4 got a
`__MATH_TG`/`__signbitf` dispatch instead), so *every* `signbit()` call on a
non-`double` goes through a conversion.

Under KLEE's SMT-LIB floating point semantics that conversion is not harmless:
`fpext` of a NaN yields the canonical NaN, which has no sign. So
`signbit(-NaN)` answers 0, and KLEE never explores the negative-NaN path at all.
Silently wrong answers, on any program compiled with this toolchain — not just
the tests.

`scripts/patches/clang-3.4-type-generic-signbit.patch` fixes it the way LLVM did
upstream after 3.4: declare the builtin with custom type checking (`"i."`) and
add it to the `SemaBuiltinFPClassification` group alongside `__builtin_isnan`.
No CodeGen change is needed — `CGBuiltin.cpp` already emits a width-generic
`bitcast`+`icmp`, and `SemaBuiltinFPClassification()` already strips the
float→double cast. `signbit()` then compiles to a sign-bit test of the actual
type (`float`→`i32`, `double`→`i64`, `long double`→`i80`). The build script
applies the patch when it unpacks Clang.

### 5. glibc's C23 `_Generic` macros collide with klee-libc

The runtime is compiled with `-D_GNU_SOURCE`, which on glibc ≥ 2.38 enables the
ISO C23 extensions. Under C23, `<string.h>` redefines `memchr`, `strchr` and
`strrchr` as `_Generic` macros:

```c
# define memchr(S, C, N) __glibc_const_generic (S, const void *, memchr (S, C, N))
```

klee-libc defines those very functions, so the macro expansion turns the
definition into a syntax error. `runtime/klee-libc/memchr.c` and `strrchr.c`
therefore `#undef` the macro after including `<string.h>` (`strchr.c` does not
include it at all, so it is unaffected).

**Do not be tempted to fix this globally**, e.g. with a `-include` header that
turns `__GLIBC_USE_ISOC23` off for the whole runtime. That is what this build
did first, and it is subtly wrong: such a header has to pull in `<features.h>`,
and `runtime/POSIX/fd_64.c` sets `_FILE_OFFSET_BITS` *after* its own includes
start. `<features.h>` has an include guard, so the early include freezes the
feature macros and fd_64.c silently compiles as the 32-bit model — defining
`open`, `stat`, `lseek` instead of `open64`, `stat64`, `lseek64`. The whole
POSIX runtime then collides with `fd_32.c` at link time, for symptoms
(`Linking globals named 'open': symbol multiply defined!`) that point nowhere
near the cause.

### 6. glibc removed the `__xstat` family, so fd_64.c stopped renaming

`runtime/POSIX/fd_32.c` and `fd_64.c` both define `__xstat`, `__lxstat` and
`__fxstat`. That used to be fine: `<sys/stat.h>` declared them, and under
`_FILE_OFFSET_BITS=64` glibc's `__REDIRECT` renamed the definitions in fd_64.c
to `__xstat64`/`__lxstat64`/`__fxstat64`.

glibc 2.33 dropped the `__xstat()`/`_STAT_VER` interface entirely — the
declarations are gone, so no redirect happens and both files define the same
three symbols. Any test pulling both modules out of `libkleeRuntimePOSIX.bca`
fails to link. fd_64.c now asks for the 64-bit names explicitly with `__asm__`
labels, guarded by a glibc version check, doing exactly what `__REDIRECT` used
to do.

### 7. klee-uclibc hardcodes `-I/usr/include`

`Rules.mak` appends `-I$(KERNEL_HEADERS) -I/usr/include` to `CFLAGS`. With
`KERNEL_HEADERS=/usr/include` (what its `configure` detects), glibc's
`<limits.h>` wins the `#include_next` race out of uClibc's own `limits.h` and
fails on undefined `__GLIBC_USE()`.

The build script drops the hardcoded `-I/usr/include` from the klee-uclibc
checkout and points `KERNEL_HEADERS` at `deps/kernel-headers`, a directory of
symlinks to just the kernel header trees (`linux/`, `asm/`, `asm-generic/`, …)
with none of glibc's top-level headers.

## Source changes on this branch

Build fixes:

* **`runtime/POSIX/fd_64.c`** — glibc ≥ 2.30 declares `getdents64` itself in
  `<bits/dirent_ext.h>` with a different prototype, so the definition no longer
  compiles. Matched to glibc's `ssize_t getdents64(int, void *, size_t)`,
  guarded by a version check. Same file: the `__xstat` asm labels from §6.

* **`runtime/klee-libc/memchr.c`, `strrchr.c`** — `#undef` glibc's C23
  `_Generic` macro before defining the function, per §5.

* **`tools/klee-replay/file-creator.c`** — glibc 2.42 removed the System V
  `struct termio` / `TCGETA` / `TCSETA` interface (there is no `<termio.h>` any
  more). Switched to the POSIX `struct termios` with `tcgetattr`/`tcsetattr`,
  which is equivalent here and available on old and new systems alike.

* **`test/Concrete/ConcreteTest.py`** — shebang was `#!/usr/bin/python`, and
  modern distributions ship no `python` binary. Changed to
  `#!/usr/bin/env python3`; the script has supported Python 3 since commit
  `a2c2e448`. Without this all 21 `Concrete/*` tests fail with exit code 127.

Expected-output updates:

* **`test/Expr/print-smt-{let,named,none}.smt2.good`,
  `test/Solver/AShr_to_smtlib.kquery.good.smt2`** — commit `aa907655` ("Add fp
  to smt printer") made `ExprSMTLIBPrinter::printSetLogic()` emit `ALL` rather
  than `QF_AUFBV`, which it has to, since the FP theory is not in `QF_AUFBV` —
  but left these files expecting the old logic. Only the `(set-logic …)` lines
  change; the rest of the generated output was already byte-identical.

* **`test/Floats/isnormal_{float,double,long_double}.c`** — expected path count
  5 → 2. glibc ≥ 2.32 implements `isnormal()` as `__builtin_isnormal()`, which
  the compiler expands inline to a couple of comparisons; it used to dispatch to
  `__fpclassify()`, which KLEE modelled and which forked four ways (0, NaN, Inf,
  subnormal). The count was always an artefact of that implementation. The
  assertions the test actually cares about — that `isnormal(x)` agrees with
  `klee_is_normal_float(x)` for every input, with no concretisation — hold
  either way, and still do.

  Note this is genuinely different from the `signbit` case in §4, which was
  fixed rather than re-baselined: there the compiled program computed a *wrong
  answer*, whereas here it computes the right answer along fewer paths.

## Test suite status

With the default (Z3) backend:

```
Total Discovered Tests: 281
  Passed           : 278
  Failed           :   0
  Unsupported      :   1
  Expectedly Failed:   2
```

With `--solver-backend=stp`: 277 passed, plus the `FastCexSolver.kquery`
invocation artefact described above.

## Things that look tempting but are not

* **Using the distribution's Z3** (4.15 here) almost works — every `Z3_mk_fpa_*`
  entry point KLEE uses still exists, and `rewriter.hi_fp_unspecified` is still
  accepted — but `Z3_TRUE` is gone, so it needs a source patch, and the solver
  is nine years newer than the one the published results were produced with.
  Building 4.5.0 is cheaper than reasoning about that.

* **Using prebuilt LLVM 3.4 binaries** from `releases.llvm.org` saves the build,
  but those archives were compiled by GCC 4.8 with the pre-C++11 libstdc++ ABI,
  so KLEE would have to be built with `-D_GLIBCXX_USE_CXX11_ABI=0` to link
  against them; they do not ship `FileCheck`/`not`/`count`, which the lit suite
  needs; and they cannot carry the `__builtin_signbit` fix from §4.

* **A global `-include` compat header** for the runtime — see the warning in §5.
  It looks like the tidy way to handle glibc's C23 macros and it quietly breaks
  the 64-bit POSIX model instead.

* **A sysroot with 2017-vintage glibc headers** would sidestep §5, §6 and §7
  wholesale. It is a bigger moving part than the fixes here and no longer buys
  anything now that the tests pass, but it remains the right answer if you ever
  need results that are bit-comparable with the original 2017 experiments.
