/* <limits.h> — bcc built-in header (ISO C11).
 *
 * Values describe bcc's target: x86-64 SysV, LP64, two's complement,
 * signed plain char.
 *
 * POSIX and system extensions (PATH_MAX and friends) live in the C
 * library's limits.h; bcc's built-in tier precedes the angled search
 * list, so this header fully shadows it for now.
 */

#ifndef __BCC_LIMITS_H
#define __BCC_LIMITS_H

#define CHAR_BIT 8

#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255

/* Plain char is signed on x86-64 SysV. */
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX

#define MB_LEN_MAX 16

#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535

#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U

#define LONG_MIN (-9223372036854775807L - 1)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 18446744073709551615UL

#define LLONG_MIN (-9223372036854775807LL - 1)
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

#endif /* __BCC_LIMITS_H */
