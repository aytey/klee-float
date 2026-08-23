#!/bin/bash
#
# Build klee-float on a 2026-era Linux distribution.
#
# This branch targets LLVM/Clang 3.4 and a 2016-era Z3, neither of which any
# current distribution ships, so both are built from source here.  Everything
# lands under $DEPS; nothing is installed system-wide and root is not needed.
#
# See BUILD_2026.md for what each step works around and why.
#
# Layout (override with KLEE_FLOAT_ROOT):
#   $ROOT/klee-float    this checkout
#   $ROOT/deps          LLVM 3.4.2, Z3 4.5.0, klee-uclibc, CMake 3.x, lit, shims
#   $ROOT/klee-build    the KLEE build tree
#
# Prerequisites:
#   gcc7 / gcc7-c++     LLVM 3.4 and Z3 4.5.0 do not build with GCC 11+
#   python3, git, curl, make, ncurses and zlib development headers
#
set -euo pipefail

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOT=${KLEE_FLOAT_ROOT:-$(dirname "$SRC")}
DEPS=$ROOT/deps
PREFIX=$DEPS/install
BUILD=$ROOT/klee-build
JOBS=${JOBS:-$(nproc)}

# Host compiler used for LLVM, Z3 and KLEE itself.  GCC 7 is the newest GCC
# that still compiles the 2014/2016 vintage of LLVM and Z3 we need.
HOST_CC=${HOST_CC:-gcc-7}
HOST_CXX=${HOST_CXX:-g++-7}

# STP is C++17 and does not build with GCC 7, so it gets the system compiler.
# That is safe because KLEE only ever talks to STP through its C interface
# (stp/c_interface.h): no C++ types cross the boundary.  See STP_STDCXX below.
STP_CC=${STP_CC:-cc}
STP_CXX=${STP_CXX:-c++}
# STP is upstream stp/stp; pin the commit that this was developed against.
# Point STP_SRC at a local checkout to build that instead.
STP_URL=${STP_URL:-https://github.com/stp/stp}
STP_COMMIT=${STP_COMMIT:-97717546cfd064100b59d89ffc3c7f1438c5f1aa}
STP_SRC=${STP_SRC:-$DEPS/stp}
# Link KLEE against the libstdc++ that will actually be loaded at run time.
# GCC 7 links against its own, older, copy, which lacks the GLIBCXX_3.4.26+
# symbols that a libstp built by a current GCC needs -- so the link fails even
# though the loader would resolve them from the system library just fine.
STP_STDCXX=${STP_STDCXX:-/usr/lib64/libstdc++.so.6}

# Where that GCC keeps crtbegin.o / libgcc, and its libstdc++ headers.  Clang
# 3.4 cannot find either on its own on a modern distribution.
GCC7_LIBDIR=${GCC7_LIBDIR:-$(${HOST_CC} -print-libgcc-file-name | xargs dirname)}
GCC7_CXX_INC=${GCC7_CXX_INC:-/usr/include/c++/7}
GCC7_CXX_INC_TARGET=${GCC7_CXX_INC_TARGET:-$GCC7_CXX_INC/$(${HOST_CC} -dumpmachine)}

mkdir -p "$DEPS/src" "$PREFIX" "$DEPS/shim-bin"

###############################################################################
# 0. Helper shims
###############################################################################

# LLVM 3.4's configure and klee-uclibc's configure both want a `python` binary.
cat > "$DEPS/shim-bin/python" <<'EOF'
#!/bin/sh
exec /usr/bin/python3 "$@"
EOF

# `make` wrapper for KLEE's runtime (bitcode) build.
# runtime/CMakeLists.txt runs `env MAKEFLAGS="" make -f Makefile.cmake.bitcode
# all`.  Modern CMake escapes the quotes, so make receives the literal
# two-character value '""' and dies with `invalid option -- '"'`.
#
# Note: do NOT be tempted to also inject a global -include header here (for
# instance to defuse glibc's C23 _Generic macros).  runtime/POSIX/fd_64.c sets
# _FILE_OFFSET_BITS before its own includes, so anything that pulls <features.h>
# in earlier makes that file silently compile as the 32-bit model and define
# open/stat/lseek instead of open64/stat64/lseek64.
cat > "$DEPS/shim-bin/make-clean-flags" <<'EOF'
#!/bin/sh
MAKEFLAGS=
export MAKEFLAGS
exec /usr/bin/make "$@"
EOF

# clang 3.4 wrapper (KLEE's LLVMCC, and klee-uclibc's bitcode compiler).
# Clang 3.4 predates the GCC layout of a current distribution, so on its own it
# finds neither crtbegin.o nor libgcc when it has to link a native binary --
# which KLEE's Concrete tests and klee-uclibc's configure checks both do.
#
# -B must NOT be in effect for the driver's "-print-*" queries: klee-uclibc
# runs `$(CC) -print-file-name=include` to locate the compiler's own header
# directory, and under -B that answers GCC's include directory instead of
# clang's, which breaks the #include_next chain out of uClibc's limits.h.
cat > "$DEPS/shim-bin/klee-clang" <<EOF
#!/bin/sh
for arg in "\$@"; do
  case \$arg in
    -print-*) exec $PREFIX/bin/clang "\$@" ;;
  esac
done
exec $PREFIX/bin/clang -B$GCC7_LIBDIR -L$GCC7_LIBDIR "\$@"
EOF

# clang++ 3.4 wrapper (KLEE's LLVMCXX).  Same native-link fix, plus: clang 3.4
# does not recognise the layout or version of a current libstdc++ and so finds
# no C++ headers at all.  GCC 7's libstdc++ headers it can still parse.
cat > "$DEPS/shim-bin/klee-clang++" <<EOF
#!/bin/sh
for arg in "\$@"; do
  case \$arg in
    -print-*) exec $PREFIX/bin/clang++ "\$@" ;;
  esac
done
exec $PREFIX/bin/clang++ -B$GCC7_LIBDIR -L$GCC7_LIBDIR \\
  -I$GCC7_CXX_INC -I$GCC7_CXX_INC_TARGET "\$@"
EOF

chmod +x "$DEPS"/shim-bin/*
export PATH="$DEPS/shim-bin:$PATH"

###############################################################################
# 1. CMake 3.x
#
# KLEE 1.3 does `cmake_policy(SET CMP0054 OLD)`.  CMake 4.x removed the OLD
# behaviour of every policy introduced before 3.5, so it refuses outright.
###############################################################################
CMAKE=$DEPS/cmake-3.31.6-linux-x86_64/bin/cmake
if [ ! -x "$CMAKE" ]; then
  curl -sSL -o "$DEPS/src/cmake-3.31.6-linux-x86_64.tar.gz" \
    https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz
  tar -C "$DEPS" -xf "$DEPS/src/cmake-3.31.6-linux-x86_64.tar.gz"
fi

###############################################################################
# 2. lit (the LLVM 3.4 tree ships a Python 2 only lit)
###############################################################################
if [ ! -x "$DEPS/pyenv/bin/lit" ]; then
  python3 -m venv "$DEPS/pyenv"
  "$DEPS/pyenv/bin/pip" -q install lit tabulate
fi

###############################################################################
# 3. LLVM 3.4.2 + Clang 3.4.2
#
# Built with the autoconf build system: LLVM 3.4's CMake files use
# FindPythonInterp, which CMake 4 removed, so autoconf is the easier route.
# --enable-targets=host keeps this to the X86 backend, which is all KLEE needs.
###############################################################################
if [ ! -x "$PREFIX/bin/llvm-config" ]; then
  cd "$DEPS/src"
  [ -f llvm-3.4.2.src.tar.gz ] || curl -sSL -O https://releases.llvm.org/3.4.2/llvm-3.4.2.src.tar.gz
  [ -f cfe-3.4.2.src.tar.gz  ] || curl -sSL -O https://releases.llvm.org/3.4.2/cfe-3.4.2.src.tar.gz
  cd "$DEPS"
  [ -d llvm-3.4.2.src ] || tar xf src/llvm-3.4.2.src.tar.gz
  if [ ! -d llvm-3.4.2.src/tools/clang ]; then
    tar xf src/cfe-3.4.2.src.tar.gz
    mv cfe-3.4.2.src llvm-3.4.2.src/tools/clang
    # Clang 3.4 declares __builtin_signbit as int(double), so signbit() on a
    # float or long double converts first -- which loses the sign of a NaN
    # under KLEE's floating point semantics.  See the patch header.
    ( cd llvm-3.4.2.src && patch -p1 < "$SRC/scripts/patches/clang-3.4-type-generic-signbit.patch" )
  fi
  mkdir -p llvm-build && cd llvm-build
  ../llvm-3.4.2.src/configure \
      --prefix="$PREFIX" \
      --enable-optimized --enable-assertions \
      --enable-targets=host --disable-docs \
      CC="$HOST_CC" CXX="$HOST_CXX" CFLAGS="-w" CXXFLAGS="-w"
  make -j"$JOBS"
  make install -j"$JOBS"
  # `make install` skips the test utilities, but KLEE's lit suite needs them.
  cp Release+Asserts/bin/FileCheck Release+Asserts/bin/not \
     Release+Asserts/bin/count "$PREFIX/bin/"
fi

###############################################################################
# 4. Z3 4.5.0
#
# 4.5.0 is the version this branch was developed against: it is the first
# release with `rewriter.hi_fp_unspecified` (which lib/Solver/Z3Solver.cpp
# sets), and it still defines Z3_TRUE / Z3_bool, which later Z3 releases
# dropped.
###############################################################################
if [ ! -f "$PREFIX/lib/libz3.so" ]; then
  cd "$DEPS/src"
  [ -f z3-4.5.0.tar.gz ] || curl -sSL -o z3-4.5.0.tar.gz \
      https://github.com/Z3Prover/z3/archive/refs/tags/z3-4.5.0.tar.gz
  cd "$DEPS"
  [ -d z3-z3-4.5.0 ] || tar xf src/z3-4.5.0.tar.gz
  cd z3-z3-4.5.0
  CC="$HOST_CC" CXX="$HOST_CXX" python scripts/mk_make.py --prefix="$PREFIX"
  cd build && make -j"$JOBS" && make install
fi

###############################################################################
# 5. klee-uclibc (branch klee_uclibc_v1.0.0, matching .travis.yml)
#
# klee-uclibc's Rules.mak hardcodes `-I/usr/include` alongside the kernel
# header path.  glibc's <limits.h> then wins the #include_next race out of
# uClibc's own limits.h and fails on undefined __GLIBC_USE().  Drop it, and
# hand uClibc a directory holding only the kernel header trees.
###############################################################################
if [ ! -f "$DEPS/klee-uclibc/lib/libc.a" ]; then
  cd "$DEPS"
  [ -d klee-uclibc ] || git clone --branch klee_uclibc_v1.0.0 --depth 1 \
      https://github.com/klee/klee-uclibc.git klee-uclibc

  rm -rf kernel-headers && mkdir kernel-headers && cd kernel-headers
  for d in linux asm asm-generic mtd sound drm misc rdma video xen scsi; do
    [ -d /usr/include/$d ] && ln -s /usr/include/$d $d
  done

  cd "$DEPS/klee-uclibc"
  ./configure --make-llvm-lib \
      --with-llvm-config="$PREFIX/bin/llvm-config" \
      --with-cc="$DEPS/shim-bin/klee-clang"
  sed -i 's|^KERNEL_HEADERS=.*|KERNEL_HEADERS="'"$DEPS"'/kernel-headers"|' .config
  sed -i 's|^CFLAGS += -I$(KERNEL_HEADERS) -I/usr/include$|CFLAGS += -I$(KERNEL_HEADERS)|' Rules.mak
  make -j"$JOBS"
fi

###############################################################################
# 6. STP, and the two dependencies it needs here
#
# STP gained a floating-point theory (SymFPU based) and a C API for it, so it
# can serve as a second FP-capable solver alongside Z3.  It needs a SAT backend
# -- none is bundled, and this machine has neither CaDiCaL nor CryptoMiniSat,
# so MiniSat is built the way klee-float's own container did -- and LibBF,
# which STP fetches and builds with its own pinned/checksummed script.  That
# script writes into ./deps relative to the working directory, so it is run
# from a scratch directory here rather than inside the STP checkout.
###############################################################################
if [ ! -f "$PREFIX/lib/libminisat.so" ]; then
  cd "$DEPS"
  [ -d minisat ] || git clone --depth 1 https://github.com/stp/minisat.git minisat
  mkdir -p minisat-build && cd minisat-build
  cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$DEPS/minisat"
  make -j"$JOBS" && make install
fi

STP_CMAKE_DIR=$(dirname "$(find "$PREFIX" -name STPConfig.cmake 2>/dev/null | head -1)" 2>/dev/null || true)
if [ ! -f "${STP_CMAKE_DIR:-/nonexistent}/STPConfig.cmake" ]; then
  if [ ! -d "$STP_SRC" ]; then
    git clone "$STP_URL" "$STP_SRC"
    ( cd "$STP_SRC" && git checkout "$STP_COMMIT" )
  fi

  if [ ! -f "$DEPS/stp-deps/deps/libbf/libbf.a" ]; then
    mkdir -p "$DEPS/stp-deps" && cd "$DEPS/stp-deps"
    bash "$STP_SRC/scripts/deps/setup-libbf.sh"
  fi

  mkdir -p "$DEPS/stp-build" && cd "$DEPS/stp-build"
  CC="$STP_CC" CXX="$STP_CXX" cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_PREFIX_PATH="$PREFIX" \
      -DNOCRYPTOMINISAT=ON \
      -DUSE_MINISAT=ON \
      -DLIBBF_DIR="$DEPS/stp-deps/deps/libbf" \
      -DBUILD_SHARED_LIBS=ON \
      -DENABLE_TESTING=OFF \
      -DENABLE_PYTHON_INTERFACE=OFF \
      "$STP_SRC"
  make -j"$JOBS" && make install
  STP_CMAKE_DIR=$(dirname "$(find "$PREFIX" -name STPConfig.cmake | head -1)")
fi

###############################################################################
# 7. KLEE itself
###############################################################################
mkdir -p "$BUILD" && cd "$BUILD"
CC="$HOST_CC" CXX="$HOST_CXX" "$CMAKE" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_KLEE_ASSERTS=ON \
  -DENABLE_SOLVER_Z3=ON \
  -DZ3_INCLUDE_DIRS="$PREFIX/include" \
  -DZ3_LIBRARIES="$PREFIX/lib/libz3.so" \
  -DENABLE_SOLVER_STP=ON \
  -DSTP_DIR="$STP_CMAKE_DIR" \
  -DCMAKE_CXX_STANDARD_LIBRARIES="$STP_STDCXX" \
  -DENABLE_POSIX_RUNTIME=ON \
  -DENABLE_KLEE_UCLIBC=ON \
  -DKLEE_UCLIBC_PATH="$DEPS/klee-uclibc" \
  -DENABLE_TCMALLOC=OFF \
  -DENABLE_UNIT_TESTS=OFF \
  -DENABLE_SYSTEM_TESTS=ON \
  -DLLVM_CONFIG_BINARY="$PREFIX/bin/llvm-config" \
  -DLLVMCC="$DEPS/shim-bin/klee-clang" \
  -DLLVMCXX="$DEPS/shim-bin/klee-clang++" \
  -DMAKE_BINARY="$DEPS/shim-bin/make-clean-flags" \
  -DLIT_TOOL="$DEPS/pyenv/bin/lit" \
  "$SRC"

make -j"$JOBS"

echo
echo "klee-float built: $BUILD/bin/klee"
echo "Run the test suite with:"
echo "  cd $BUILD && PATH=$DEPS/shim-bin:\$PATH make systemtests"
echo
echo "That exercises the default (Z3) backend. To run it against STP instead:"
echo "  cd $BUILD && PATH=$DEPS/shim-bin:\$PATH $DEPS/pyenv/bin/lit -v \\"
echo "    --param klee_opts=--solver-backend=stp \\"
echo "    --param kleaver_opts=--solver-backend=stp test"
