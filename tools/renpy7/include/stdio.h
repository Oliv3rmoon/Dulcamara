/*
 * Dulcamara - the SDK's stdio.h for CPython 2.7 on the PS5 (tools/renpy7/ps5.sh puts this folder first), with its
 * character macros made calls into the console's libc. The SDK's headers are FreeBSD's: getc_unlocked and putc_unlocked
 * read and write a FILE as FreeBSD's libc lays it out and call its __srget and __swbuf, and getc, putc, feof, ferror,
 * clearerr and fileno do the same while __isthreaded is 0. The app does not link FreeBSD's libc but the console's,
 * whose __srget ended Ren'Py 7 on 0.2.9 at the first .py file Python read (its tokenizer, Py_UniversalNewlineFgets).
 * Here each is the console's own function: the _unlocked ones the locked one (a FILE's lock is taken again by the
 * thread that holds it, as flockfile allows).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DULCAMARA_STDIO_H
#define DULCAMARA_STDIO_H

#include_next <stdio.h>

#undef getc
#undef putc
#undef getchar
#undef putchar
#undef getc_unlocked
#undef putc_unlocked
#undef getchar_unlocked
#undef putchar_unlocked
#undef fputc_unlocked
#undef feof
#undef ferror
#undef clearerr
#undef fileno
#undef feof_unlocked
#undef ferror_unlocked
#undef clearerr_unlocked
#undef fileno_unlocked

#define getc_unlocked(fp) getc(fp)
#define putc_unlocked(c, fp) putc(c, fp)
#define getchar_unlocked() getchar()
#define putchar_unlocked(c) putchar(c)
#define fputc_unlocked(c, fp) fputc(c, fp)
#define feof_unlocked(fp) feof(fp)
#define ferror_unlocked(fp) ferror(fp)
#define clearerr_unlocked(fp) clearerr(fp)
#define fileno_unlocked(fp) fileno(fp)

#endif
