#!/bin/bash
#
# Run every benchmark in the issta_2017 study set under every solver
# configuration, then summarise.
#
#   run-all.sh [config ...]        (default: all five)
#
# Expects fetch-and-build.sh to have run.
#
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=$(cd "$HERE/../.." && pwd)
ROOT=${KLEE_FLOAT_ROOT:-$(dirname "$SRC")}
WORK=${FP_BENCH_ROOT:-$ROOT/fp-bench-2026}
OUT=${FP_BENCH_OUT:-$WORK/runs}
BUILD=$WORK/build_O2
# Solver processes are single threaded; leave headroom so the timings are not
# measuring contention.
PAR=${FP_BENCH_PARALLEL:-8}

CONFIGS=("$@")
if [ ${#CONFIGS[@]} -eq 0 ]; then
  CONFIGS=(z3-450 z3-415 z3-50 z3-51 stp bitwuzla)
fi

INVOKE=$BUILD/issta_invocation_info.yml
[ -f "$INVOKE" ] || { echo "missing $INVOKE -- run fetch-and-build.sh first" >&2; exit 1; }

mkdir -p "$OUT"
: > "$OUT/results.psv"
grep "^  program:" "$INVOKE" | awk '{print $2}' > "$OUT/bclist.txt"
echo "benchmarks: $(wc -l < "$OUT/bclist.txt")   configs: ${CONFIGS[*]}"

: > "$OUT/jobs.txt"
for cfg in "${CONFIGS[@]}"; do
  while read -r bc; do echo "$cfg $bc"; done < "$OUT/bclist.txt" >> "$OUT/jobs.txt"
done
echo "total runs: $(wc -l < "$OUT/jobs.txt")"

xargs -P"$PAR" -n2 "$HERE/run-one.sh" < "$OUT/jobs.txt"

echo
python3 "$HERE/aggregate.py"
