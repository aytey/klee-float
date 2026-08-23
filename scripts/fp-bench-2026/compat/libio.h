/* Compatibility shim: glibc removed <libio.h> (its contents were folded into
   <stdio.h> and the bits/types/struct_FILE.h it includes).  libmatheval 1.1.11
   includes it for the _IO_FILE machinery its flex-generated scanner refers to,
   all of which <stdio.h> still provides. */
#ifndef _LIBIO_H_COMPAT_SHIM
#define _LIBIO_H_COMPAT_SHIM 1
#include <stdio.h>
#include <stdarg.h>
#ifndef _G_va_list
# define _G_va_list va_list
#endif
#endif
