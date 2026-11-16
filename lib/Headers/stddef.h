/* <stddef.h> — bcc built-in header (ISO C11, freestanding).
 *
 * bcc targets x86-64 SysV (LP64): size_t is unsigned long, ptrdiff_t is
 * long, wchar_t is int, and plain char is signed.
 *
 * This header also implements the GNU __need_ protocol used by glibc: a
 * requester may define __need_size_t (etc.) before including it to obtain
 * exactly one definition. The compound outer conditional — deliberately
 * not a plain #ifndef guard — defeats the preprocessor's include-guard
 * multiple-include optimization, so a __need_ re-include is always
 * processed rather than skipped.
 */

#if !defined(__BCC_STDDEF_H) || defined(__need_ptrdiff_t) \
    || defined(__need_size_t) || defined(__need_wchar_t)  \
    || defined(__need_wint_t) || defined(__need_NULL)

/* Full mode: included directly, with no __need_ macro in effect. */
#if !defined(__BCC_STDDEF_H) && !defined(__need_ptrdiff_t) \
    && !defined(__need_size_t) && !defined(__need_wchar_t) \
    && !defined(__need_wint_t) && !defined(__need_NULL)
#define __BCC_STDDEF_H
#define __BCC_STDDEF_ALL
#endif

#if defined(__BCC_STDDEF_ALL) || defined(__need_ptrdiff_t)
#ifndef __BCC_PTRDIFF_T
#define __BCC_PTRDIFF_T
typedef long ptrdiff_t;
#endif
#undef __need_ptrdiff_t
#endif

#if defined(__BCC_STDDEF_ALL) || defined(__need_size_t)
#ifndef __BCC_SIZE_T
#define __BCC_SIZE_T
typedef unsigned long size_t;
#endif
#undef __need_size_t
#endif

#if defined(__BCC_STDDEF_ALL) || defined(__need_wchar_t)
#ifndef __BCC_WCHAR_T
#define __BCC_WCHAR_T
typedef int wchar_t;
#endif
#undef __need_wchar_t
#endif

#if defined(__BCC_STDDEF_ALL) || defined(__need_wint_t)
#ifndef __BCC_WINT_T
#define __BCC_WINT_T
typedef unsigned int wint_t;
#endif
#undef __need_wint_t
#endif

#if defined(__BCC_STDDEF_ALL) || defined(__need_NULL)
#undef NULL
#define NULL ((void*)0)
#undef __need_NULL
#endif

#if defined(__BCC_STDDEF_ALL)

#define offsetof(type, member) ((size_t)&(((type*)0)->member))

/* C11: a type whose alignment is at least as strict as that of every
 * fundamental type. */
typedef struct {
  long long __bcc_max_align_ll;
  long double __bcc_max_align_ld;
} max_align_t;

#endif /* __BCC_STDDEF_ALL */

#undef __BCC_STDDEF_ALL

#endif /* !__BCC_STDDEF_H || __need_... */
