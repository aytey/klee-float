#!/bin/bash
#
# Fetch and build the fp-bench floating-point benchmark suite used by
#
#   "Floating-Point Symbolic Execution: A Case Study in N-Version Programming"
#   Liew, Schemmel, Cadar, Donaldson, Zahl, Wehrle. ASE 2017.
#
# and emit the invocation info for its 86-benchmark study set.
#
# The three repositories and commits are the ones scripts/container.Makefile
# pins, i.e. what the paper's own container used.  The suite is built with the
# LLVM 3.4 toolchain that scripts/build-2026.sh produces, so run that first.
#
# See FP_BENCH_2026.md for what breaks on a 2026 system and why the workarounds
# below are needed.
#
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)                  # this klee-float checkout
ROOT=${KLEE_FLOAT_ROOT:-$(dirname "$SRC")}
DEPS=$ROOT/deps
PREFIX=$DEPS/install
KLEE_BUILD=${KLEE_BUILD:-$ROOT/klee-build}
WORK=${FP_BENCH_ROOT:-$ROOT/fp-bench-2026}
JOBS=${JOBS:-$(nproc)}

FPBENCH_URL=https://github.com/delcypher/fp-bench.git
FPBENCH_COMMIT=440b9a2402acfe5bd110f871c208808289606103
IMPERIAL_URL=https://github.com/delcypher/fp-benchmarks-imperial.git
IMPERIAL_COMMIT=f2c7c17dd9727233819a7d8598cbffd7ad4a29e0
AACHEN_URL=https://github.com/delcypher/fp-benchmarks-aachen.git
AACHEN_COMMIT=265b1195e3734dec7c12054bea9919d8d38f0b07

mkdir -p "$WORK"

###############################################################################
# 1. Fetch, at the pinned commits
###############################################################################
if [ ! -d "$WORK/fp-bench" ]; then
  git clone "$FPBENCH_URL" "$WORK/fp-bench"
  ( cd "$WORK/fp-bench" && git checkout "$FPBENCH_COMMIT" )
fi
if [ ! -d "$WORK/fp-bench/benchmarks/c/imperial" ]; then
  git clone "$IMPERIAL_URL" "$WORK/fp-bench/benchmarks/c/imperial"
  ( cd "$WORK/fp-bench/benchmarks/c/imperial" && git checkout "$IMPERIAL_COMMIT" )
fi
if [ ! -d "$WORK/fp-bench/benchmarks/c/aachen" ]; then
  git clone "$AACHEN_URL" "$WORK/fp-bench/benchmarks/c/aachen"
  ( cd "$WORK/fp-bench/benchmarks/c/aachen" && git checkout "$AACHEN_COMMIT" )
fi

###############################################################################
# 2. Python tooling
#
# wllvm produces the whole-program bitcode; fp-bench's own svcb tooling needs
# PyYAML and the jsonschema version its schema was written against.
###############################################################################
if [ ! -x "$DEPS/pyenv/bin/wllvm" ]; then
  "$DEPS/pyenv/bin/pip" -q install wllvm PyYAML "jsonschema==2.5.1"
fi

# wllvm calls the bitcode compiler and the LLVM binutils by name out of
# LLVM_COMPILER_PATH, so make sure the LLVM 3.4 tools are reachable there
# alongside the klee-clang wrappers.
for t in llvm-link llvm-ar llvm-as llvm-dis llvm-nm; do
  ln -sf "$PREFIX/bin/$t" "$DEPS/shim-bin/$t"
done

export PATH="$DEPS/pyenv/bin:$DEPS/shim-bin:$PATH"
export LLVM_COMPILER=clang
export LLVM_COMPILER_PATH="$DEPS/shim-bin"
export LLVM_CC_NAME=klee-clang
export LLVM_CXX_NAME=klee-clang++
export KLEE_NATIVE_RUNTIME_INCLUDE_DIR="$SRC/include"
export KLEE_NATIVE_RUNTIME_LIB_DIR="$KLEE_BUILD/lib"
# glibc removed <libio.h>, <_G_config.h> and <xlocale.h>; the vendored
# libmatheval 1.1.11 still includes them.  CPATH injects replacements without
# touching the benchmark sources.
export CPATH="$HERE/compat${CPATH:+:$CPATH}"

###############################################################################
# 3. Configure and build
#
# RelWithDebInfo is the -O2 build (what Imperial used); pass
# -DCMAKE_BUILD_TYPE=Debug for the -O0 build Aachen used.
###############################################################################
BUILD=$WORK/build_O2
mkdir -p "$BUILD" && cd "$BUILD"
CC=wllvm CXX=wllvm++ "$DEPS/cmake-3.31.6-linux-x86_64/bin/cmake" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_IMPERIAL_BENCHMARKS=ON \
  -DBUILD_AACHEN_BENCHMARKS=ON \
  "$WORK/fp-bench"

# GSL and GMP are built as external projects and, on a distribution that uses
# lib64, install there -- while fp-bench looks for lib/libgsl.a and
# lib/libgmp.a.  The externals have to have been built for these to exist, so
# this runs as a build/link/build cycle rather than up front.
set +e
make -j"$JOBS" all create-augmented-spec-file-list
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
  linked=0
  for d in $(find "$BUILD" -type d -name lib64); do
    p=$(dirname "$d")
    if [ ! -e "$p/lib" ]; then ln -s lib64 "$p/lib"; linked=1; fi
  done
  if [ "$linked" -eq 1 ]; then
    echo "fp-bench: linked lib -> lib64 for vendored libraries, resuming build"
    make -j"$JOBS" all create-augmented-spec-file-list
  else
    echo "fp-bench: build failed and no lib64/lib mismatch to fix" >&2
    exit 1
  fi
fi

###############################################################################
# 4. Emit the study set
###############################################################################
python "$WORK/fp-bench/svcb/tools/filter-augmented-spec-list.py" \
  --categories issta_2017 -- augmented_spec_files.txt > issta_augmented_spec_files.txt
python "$WORK/fp-bench/svcb/tools/svcb-emit-klee-runner-invocation-info.py" \
  issta_augmented_spec_files.txt -o issta_invocation_info.yml

echo
echo "all benchmarks built      : $(wc -l < augmented_spec_files.txt)"
echo "in the issta_2017 study set: $(wc -l < issta_augmented_spec_files.txt)"
echo "invocation info            : $BUILD/issta_invocation_info.yml"
