#!/bin/bash
#
# Run one fp-bench benchmark under one solver configuration.
#
#   run-one.sh <config> <benchmark.bc>
#
# Configurations: "stp", "bitwuzla", plus one per Z3 build
# under test. KLEE is linked against the Z3 that scripts/build-2026.sh builds
# (4.5.0); the other Z3 builds are swapped in at run time via LD_LIBRARY_PATH,
# which works because KLEE only uses Z3's C API. Point Z3_<name>_LIB at a
# libz3.so.* to add or move one.
#
# Appends one pipe-separated result row to $OUT/results.psv:
#   config|benchmark|exit code|wall seconds|#.err files|#"KLEE: done:" lines
#
# See FP_BENCH_2026.md for why the KLEE options below are what they are.
#
set -u

CFG=$1
BC=$2

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
ROOT=${KLEE_FLOAT_ROOT:-$(dirname "$SRC")}
KLEE=${KLEE:-$ROOT/klee-build/bin/klee}
WORK=${FP_BENCH_ROOT:-$ROOT/fp-bench-2026}
OUT=${FP_BENCH_OUT:-$WORK/runs}

# Exploration budget, and the hard kill that backs it up. KLEE's --max-time
# cannot fire while a single solver query is still running -- the very problem
# the paper's "dynamic solver timeout" addressed -- so a long query can run well
# past it. The outer timeout bounds that, identically for every solver.
SOFT=${FP_BENCH_SOFT_TIMEOUT:-60}
HARD=${FP_BENCH_HARD_TIMEOUT:-150}

# STP builds to compare, one per SAT backend. STP prefers CaDiCaL over
# CryptoMiniSat over MiniSat when several are compiled in (UserDefinedFlags.h),
# so each of these must be a build with exactly one enabled. Empty value => use
# whatever KLEE was linked against.
STP_stp_LIB=${STP_LIB:-}
STP_stp_minisat_LIB=${STP_MINISAT_LIB:-}
STP_stp_cadical2_LIB=${STP_CADICAL2_LIB:-}
STP_stp_cadical3_LIB=${STP_CADICAL3_LIB:-}
STP_stp_cmsat_LIB=${STP_CMSAT_LIB:-}
# Extra directories the swapped-in STP itself needs (CryptoMiniSat is a shared
# library, unlike the statically linked CaDiCaL and the installed MiniSat).
STP_EXTRA_LIBDIRS=${STP_EXTRA_LIBDIRS:-}

# Z3 builds to compare. Empty value => use whatever KLEE was linked against.
Z3_z3_450_LIB=${Z3_450_LIB:-}
Z3_z3_415_LIB=${Z3_415_LIB:-/usr/lib64/libz3.so.4.15}
Z3_z3_50_LIB=${Z3_50_LIB:-$HOME/clones/z3/master/build/libz3.so.5.0.0.0}
Z3_z3_51_LIB=${Z3_51_LIB:-$HOME/clones/z3/master/build-gcc16-py311/libz3.so.5.1.0.0}

case $CFG in
  stp)           BACKEND=stp; LIB=$STP_stp_LIB;          SONAME=libstp.so.2.4 ;;
  stp-minisat)   BACKEND=stp; LIB=$STP_stp_minisat_LIB;  SONAME=libstp.so.2.4 ;;
  stp-cadical2)  BACKEND=stp; LIB=$STP_stp_cadical2_LIB; SONAME=libstp.so.2.4 ;;
  stp-cadical3)  BACKEND=stp; LIB=$STP_stp_cadical3_LIB; SONAME=libstp.so.2.4 ;;
  stp-cmsat)     BACKEND=stp; LIB=$STP_stp_cmsat_LIB;    SONAME=libstp.so.2.4 ;;
  bitwuzla)      BACKEND=bitwuzla; LIB="" ;;
  z3-450) BACKEND=z3;  LIB=$Z3_z3_450_LIB; SONAME=libz3.so ;;
  z3-415) BACKEND=z3;  LIB=$Z3_z3_415_LIB; SONAME=libz3.so ;;
  z3-50)  BACKEND=z3;  LIB=$Z3_z3_50_LIB;  SONAME=libz3.so ;;
  z3-51)  BACKEND=z3;  LIB=$Z3_z3_51_LIB;  SONAME=libz3.so ;;
  *) echo "unknown config: $CFG" >&2; exit 2 ;;
esac

name=$(basename "$BC" .bc)
out=$OUT/$CFG/$name
mkdir -p "$(dirname "$out")" "$OUT"
rm -rf "$out"

# A swapped-in Z3 needs to be found under the soname KLEE was linked against
# ("libz3.so"), so give each one a directory containing just that symlink.
if [ -n "$LIB" ]; then
  libdir=$OUT/.solverlib/$CFG
  mkdir -p "$libdir"
  ln -sf "$LIB" "$libdir/$SONAME"
  export LD_LIBRARY_PATH=$libdir${STP_EXTRA_LIBDIRS:+:$STP_EXTRA_LIBDIRS}
fi

s=$(date +%s.%N)
timeout -s KILL "$HARD" "$KLEE" \
    --solver-backend="$BACKEND" \
    `# only STP honours this; with it on, STP forks a process per query while` \
    `# Z3 runs in-process, which would not be a like-for-like comparison` \
    --use-forked-solver=false \
    `# without a libc these benchmarks cannot resolve stdout/fprintf and barely` \
    `# execute; --posix-runtime is not usable as their main() takes no arguments` \
    --libc=uclibc \
    --max-time="$SOFT" \
    --max-memory=4000 \
    --output-dir="$out" \
    "$BC" > "$out.log" 2>&1
rc=$?
e=$(date +%s.%N)

# assembly.ll is by far the largest output and nothing here reads it
rm -f "$out/assembly.ll"

errs=$(ls "$out"/*.err 2>/dev/null | wc -l)
done_lines=$(grep -c "KLEE: done:" "$out.log" 2>/dev/null)
echo "$CFG|$name|$rc|$(echo "$e-$s" | bc)|$errs|$done_lines" >> "$OUT/results.psv"
