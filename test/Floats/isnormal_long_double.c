// RUN: %llvmgcc %s -emit-llvm -O0 -g -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --exit-on-error %t1.bc > %t-output.txt 2>&1
// RUN: FileCheck -input-file=%t-output.txt %s
// REQUIRES: x86_64
#include "klee/klee.h"
#include <stdio.h>
#include <math.h>
#include <assert.h>

int main() {
  long double x;
  klee_make_symbolic(&x, sizeof(long double), "x");
  if (klee_is_normal_long_double(x)) {
    assert(isnormal(x));
  } else {
    assert(!klee_is_normal_long_double(x));
    // glibc >= 2.32 implements isnormal() as __builtin_isnormal(), which the
    // compiler expands inline to a couple of comparisons.  It used to dispatch
    // to __fpclassify(), which KLEE modelled and which forked four ways here
    // (0, NaN, Inf, subnormal), for five completed paths in total.  Either way
    // the assertions below are what matters: they hold for every input.
    assert(!isnormal(x));
  }
}
// CHECK-NOT: silently concretizing (reason: floating point)
// CHECK: KLEE: done: completed paths = 2
