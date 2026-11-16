/* <float.h> — bcc built-in header (ISO C11).
 *
 * Values describe IEEE-754 binary32 / binary64 as implemented by bcc.
 * bcc lowers long double to double (a documented deviation from the
 * x86-64 SysV 80-bit extended format), so the LDBL_ macros describe the
 * binary64 model the compiler actually implements.
 */

#ifndef __BCC_FLOAT_H
#define __BCC_FLOAT_H

#define FLT_RADIX 2

#define FLT_MANT_DIG 24
#define DBL_MANT_DIG 53
#define LDBL_MANT_DIG 53

#define DECIMAL_DIG 17

#define FLT_DIG 6
#define DBL_DIG 15
#define LDBL_DIG 15

#define FLT_MIN_EXP (-125)
#define DBL_MIN_EXP (-1021)
#define LDBL_MIN_EXP (-1021)

#define FLT_MIN_10_EXP (-37)
#define DBL_MIN_10_EXP (-307)
#define LDBL_MIN_10_EXP (-307)

#define FLT_MAX_EXP 128
#define DBL_MAX_EXP 1024
#define LDBL_MAX_EXP 1024

#define FLT_MAX_10_EXP 38
#define DBL_MAX_10_EXP 308
#define LDBL_MAX_10_EXP 308

#define FLT_MAX 3.40282347e+38F
#define DBL_MAX 1.7976931348623157e+308
#define LDBL_MAX 1.7976931348623157e+308L

#define FLT_EPSILON 1.19209290e-7F
#define DBL_EPSILON 2.2204460492503131e-016
#define LDBL_EPSILON 2.2204460492503131e-016L

#define FLT_MIN 1.17549435e-38F
#define DBL_MIN 2.2250738585072014e-308
#define LDBL_MIN 2.2250738585072014e-308L

#define FLT_ROUNDS 1
#define FLT_EVAL_METHOD 0

#define FLT_HAS_SUBNORM 1
#define DBL_HAS_SUBNORM 1
#define LDBL_HAS_SUBNORM 1

#endif /* __BCC_FLOAT_H */
