/*===-- strrchr.c ---------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===*/

#include <string.h>

/* Modern glibc turns on its ISO C23 extensions under _GNU_SOURCE, and under
   C23 <string.h> defines strrchr() as a _Generic macro.  We are defining the
   function itself, so drop the macro first. */
#ifdef strrchr
#undef strrchr
#endif

char *strrchr(const char *t, int c) {
  char ch;
  const char *l=0;

  ch = c;
  for (;;) {
    if (*t == ch) l=t; if (!*t) return (char*)l; ++t;
  }
  return (char*)l;
}
