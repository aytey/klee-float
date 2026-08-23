# Building klee-float in 2026

This branch is KLEE 1.3 plus symbolic floating point, written against a 2017
toolchain: **LLVM/Clang 3.4** and a **2016-era Z3**. No current distribution
ships either, and neither builds with a current GCC, so both have to be built
from source. This document records what is needed and, more usefully, *why* —
every workaround below is a concrete incompatibility between this code and a
2026 Linux system, not a matter of taste.

Verified on openSUSE Tumbleweed (glibc 2.43, GCC 11 default, CMake 4.1).

## Quick start

```sh
scripts/build-2026.sh
```

Everything is built out-of-tree and nothing is installed system-wide:

```
$ROOT/klee-float    this checkout
$ROOT/deps          LLVM 3.4.2, Z3 4.5.0, klee-uclibc, CMake 3.31, lit, wrapper scripts
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
| `git`, `curl`, `make` | Fetching and building the dependencies. |
| ncurses and zlib development headers | LLVM, and klee-uclibc's `configure` probe. |

## Dependency versions, and why those

| Dependency | Version | Reason |
| --- | --- | --- |
| LLVM + Clang | **3.4.2** | `.travis.yml` and the `Dockerfile` pin LLVM 3.4; the source is full of `LLVM_VERSION_CODE < LLVM_VERSION(3,5)` guards. 3.4.2 is the last 3.4 point release, matching the `release_34` branch the original Docker image built from. |
| Z3 | **4.5.0** | `lib/Solver/Z3Solver.cpp` sets `rewriter.hi_fp_unspecified`, first available in 4.5.0. It is also late enough to have the full `Z3_mk_fpa_*` API and early enough to still define `Z3_TRUE`/`Z3_bool`, which later Z3 releases removed (`Z3Solver.cpp` uses `Z3_TRUE` directly). |
| klee-uclibc | `klee_uclibc_v1.0.0` | The branch named in `.travis.yml` for the LLVM 3.4 configurations. |
| CMake | **3.31.6** (a local binary tarball) | See below — CMake 4 refuses to configure this project at all. |
| lit | current, in a venv | The `utils/lit` in the LLVM 3.4 tree is Python 2 only. Modern lit runs this test suite fine. |

Only the X86 backend is built (`--enable-targets=host`); it is all KLEE needs.

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

### 3. glibc's C23 `_Generic` macros collide with klee-libc

The runtime is compiled with `-D_GNU_SOURCE`, which on glibc ≥ 2.38 enables the
ISO C23 extensions. Under C23, `<string.h>` redefines `memchr`, `strchr` and
`strrchr` as `_Generic` macros:

```c
# define memchr(S, C, N) __glibc_const_generic (S, const void *, memchr (S, C, N))
```

klee-libc defines those very functions, in K&R style, so the macro expansion
turns the definition into a syntax error.

The wrapper injects `deps/compat/glibc-c23-off.h` through the bitcode build
system's own `LLVMCC.ExtraFlags` hook. It includes `<features.h>` (idempotent,
it has an include guard) and then switches `__GLIBC_USE_ISOC23` back off, so no
later header sees the C23 extensions.

Note this specifically is *not* solved by pre-including `<string.h>` or
`<dirent.h>` and undefining things: `runtime/POSIX/fd_64.c` sets
`_FILE_OFFSET_BITS 64` before its own includes, and on x86-64 that genuinely
changes the declarations glibc emits (`readdir` gains an `__asm__("readdir64")`
redirect, `struct dirent` switches to `__ino64_t`/`__off64_t`). Pulling headers
in early would silently compile that file against the wrong set.

### 4. Clang 3.4 cannot find any modern GCC

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

### 5. klee-uclibc hardcodes `-I/usr/include`

`Rules.mak` appends `-I$(KERNEL_HEADERS) -I/usr/include` to `CFLAGS`. With
`KERNEL_HEADERS=/usr/include` (what its `configure` detects), glibc's
`<limits.h>` wins the `#include_next` race out of uClibc's own `limits.h` and
fails on undefined `__GLIBC_USE()`.

The build script drops the hardcoded `-I/usr/include` from the klee-uclibc
checkout and points `KERNEL_HEADERS` at `deps/kernel-headers`, a directory of
symlinks to just the kernel header trees (`linux/`, `asm/`, `asm-generic/`, …)
with none of glibc's top-level headers.

## Source changes on this branch

Three real incompatibilities could not be handled from outside the tree:

* **`runtime/POSIX/fd_64.c`** — glibc ≥ 2.30 declares `getdents64` itself in
  `<bits/dirent_ext.h>` (pulled in by `<dirent.h>` under `_GNU_SOURCE`), with a
  different prototype from the runtime's, so the definition no longer compiles.
  The definition now matches glibc's `ssize_t getdents64(int, void *, size_t)`,
  guarded by a glibc version check so older systems keep the original.

* **`tools/klee-replay/file-creator.c`** — glibc 2.42 removed the System V
  `struct termio` / `TCGETA` / `TCSETA` interface (there is no `<termio.h>` any
  more). Switched to the POSIX `struct termios` with `tcgetattr`/`tcsetattr`,
  which is equivalent here and available on old and new systems alike.

* **`test/Concrete/ConcreteTest.py`** — shebang was `#!/usr/bin/python`, and
  modern distributions ship no `python` binary. Changed to
  `#!/usr/bin/env python3`; the script has supported Python 3 since commit
  `a2c2e448`. Without this all 21 `Concrete/*` tests fail with exit code 127.

## Test suite status

`make systemtests` on this configuration:

```
Total Discovered Tests: 281
  Passed           : 265
  Failed           :  12
  Unsupported      :   2
  Expectedly Failed:   2
```

None of the 12 failures are caused by the port. They fall into three groups.

### Stale expected output in the SMT printer tests (4)

`Expr/print-smt-let.kquery`, `Expr/print-smt-named.kquery`,
`Expr/print-smt-none.kquery`, `Solver/AShr_to_smtlib.kquery`.

**Pre-existing breakage on this branch.** Commit `aa907655` ("Add fp to smt
printer") made `ExprSMTLIBPrinter::printSetLogic()` emit `ALL` for both the
`QF_ABV` and `QF_AUFBV` cases — necessary, since the FP theory is not in
`QF_AUFBV` — but the `test/Expr/*.smt2.good` files still expect
`(set-logic QF_AUFBV )`. The fix is to regenerate those four expected outputs;
this branch deliberately leaves that alone as it is a semantic decision about
the printer, not a build issue.

### Floating-point tests defeated by modern glibc math macros (7)

`Floats/isnormal_{float,double,long_double}.c`,
`Floats/{,internal_}fabs_{float,long_double}_symbolic.c`.

These are environmental, and worth understanding before trusting FP results
compiled on a 2026 host:

* **`isnormal`** — glibc ≥ 2.32 defines `isnormal(x)` as `__builtin_isnormal(x)`
  for Clang ≥ 3.3 (the `__glibc_clang_prereq (3,3)` guard did not exist in
  2017, so Clang 3.4 used to get the `__MATH_TG`/`__fpclassifyf` dispatch).
  Clang 3.4 lowers the builtin to an inline `fabsf` plus two `fcmp`s instead of
  a call KLEE models, so KLEE explores 2 paths where the tests expect 5.

* **`signbit`** — same guard change, so `signbit(x)` is now
  `__builtin_signbit(x)`. Clang 3.4's `__builtin_signbit` takes a `double`, so a
  `float` argument is converted first. Under SMT-LIB FP semantics NaN has no
  sign, so the conversion canonicalises it and the negative-NaN path disappears:
  6 paths instead of the expected 7.

A `-include` shim restoring the pre-2.32 macro definitions would make these
tests pass, but it would also change what any benchmark compiled through the
same compiler means, so it is not enabled. Be aware of the effect if you are
comparing FP results against numbers produced in 2017.

### POSIX runtime symbol clash (1)

`Runtime/POSIX/DirConsistency.c`.

The test compiles the same program twice, the second time with
`-D_FILE_OFFSET_BITS=64`, and requires identical output. In the 64-bit variant,
KLEE's demand-driven linking pulls **both** `fd_32.bc` and `fd_64.bc` out of
`libkleeRuntimePOSIX.bca` — the program needs `readdir64`/`stat64`, whose uClibc
implementations reach into both — and both modules define `open` strongly:

```
KLEE: ERROR: Link with library ... failed: Linking globals named 'open': symbol multiply defined!
```

Pre-existing brittleness in how the 32-bit and 64-bit POSIX models are split,
exposed by which libc symbols this system's headers make the test reference.
Every other POSIX runtime test passes.

## Things that look tempting but are not

* **Using the distribution's Z3** (4.15 here) almost works — every `Z3_mk_fpa_*`
  entry point KLEE uses still exists, and `rewriter.hi_fp_unspecified` is still
  accepted — but `Z3_TRUE` is gone, so it needs a source patch, and the solver
  is nine years newer than the one the published results were produced with.
  Building 4.5.0 is cheaper than reasoning about that.

* **Using prebuilt LLVM 3.4 binaries** from `releases.llvm.org` saves the build,
  but those archives were compiled by GCC 4.8 with the pre-C++11 libstdc++ ABI,
  so KLEE would have to be built with `-D_GLIBCXX_USE_CXX11_ABI=0` to link
  against them, and they do not ship `FileCheck`/`not`/`count`, which the lit
  suite needs. Building from source keeps one consistent ABI and provides the
  test utilities.

* **A sysroot with 2017-vintage glibc headers** would sidestep groups 3, 4 and
  5 above wholesale, and would also make the FP tests in group 2 behave as they
  did in 2017. It is a bigger moving part than the fixes here, but it is the
  right answer if you need results that are bit-comparable with the original
  experiments.
