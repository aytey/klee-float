/* Compatibility shim: glibc removed <xlocale.h> in 2.26; the locale_t type and
   friends it declared now live in <locale.h>. */
#ifndef _XLOCALE_H_COMPAT_SHIM
#define _XLOCALE_H_COMPAT_SHIM 1
#include <locale.h>
#endif
