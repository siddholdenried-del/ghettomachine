/* The JieLi compiler provides these C builtins without libc headers. */
#ifndef FM1_V3_FREESTANDING_H
#define FM1_V3_FREESTANDING_H
#include <stddef.h>
#define V3_CLEAR __builtin_memset
#define V3_ABS __builtin_fabsf
#define V3_FINITE __builtin_isfinite
#endif
