/* <stdarg.h> — bcc built-in header (ISO C99).
 *
 * va_list is the x86-64 SysV ABI type: a one-element array of the va_list
 * control struct. The layout is part of the ABI and matches what glibc's
 * varargs prototypes expect.
 *
 * bcc does not yet lower the va_start / va_arg / va_end / va_copy
 * builtins, so they are deliberately NOT defined here: any use fails to
 * compile with a clear diagnostic instead of silently producing broken
 * code. Hosted headers that only need the __gnuc_va_list type (such as
 * glibc's <stdio.h>) parse fine.
 *
 * Like <stddef.h>, this header honors glibc's __need___va_list protocol;
 * the compound outer conditional keeps it outside the include-guard
 * multiple-include optimization so re-includes are always processed.
 */

#if !defined(__BCC_STDARG_H) || defined(__need___va_list)
#ifndef __BCC_STDARG_H
#define __BCC_STDARG_H
#endif

#ifndef __BCC_VA_LIST
#define __BCC_VA_LIST
typedef struct {
  unsigned int gp_offset;
  unsigned int fp_offset;
  void* overflow_arg_area;
  void* reg_save_area;
} __builtin_va_list[1];
#endif

#ifndef __GNUC_VA_LIST
#define __GNUC_VA_LIST
typedef __builtin_va_list __gnuc_va_list;
#endif

#if defined(__need___va_list)
#undef __need___va_list
#else
typedef __builtin_va_list va_list;
#endif

#endif /* !__BCC_STDARG_H || __need___va_list */
