#ifndef SOS2_ABI_H
#define SOS2_ABI_H
/* The SOS2 user ABIs: arm64 (LP64) and ARM32 (ILP32). Apply this before every
 * Newlib/SDK/C++ header, including while rebuilding the libraries themselves.
 * C++ libraries built without this header are not part of the SOS2 SDK ABI. */
#if defined(__aarch64__)
#if __SIZEOF_INT__ != 4 || __SIZEOF_LONG__ != 8 || __SIZEOF_POINTER__ != 8
#error "This SDK implements the SOS2 arm64 LP64 ABI"
#endif
/* AArch64 GCC already uses int/unsigned int for the 32-bit integer types. */
#else
/* SOS2 ARM32 uses int/unsigned int for exact, least and fast 32-bit integer
 * types. The generic bare-metal GCC distribution defaults to long instead.
 * Width, alignment and ARM register ABI stay 32-bit. */
#if !defined(__arm__) || __SIZEOF_INT__ != 4 || __SIZEOF_LONG__ != 4
#error "This SDK implements the SOS2 ARM32 ILP32 ABI"
#endif
#undef __INT32_TYPE__
#undef __UINT32_TYPE__
#undef __INT_LEAST32_TYPE__
#undef __UINT_LEAST32_TYPE__
#undef __INT_FAST32_TYPE__
#undef __UINT_FAST32_TYPE__
#define __INT32_TYPE__        int
#define __UINT32_TYPE__       unsigned int
#define __INT_LEAST32_TYPE__  int
#define __UINT_LEAST32_TYPE__ unsigned int
#define __INT_FAST32_TYPE__   int
#define __UINT_FAST32_TYPE__  unsigned int
#undef __INT32_C
#undef __UINT32_C
#define __INT32_C(value)  value
#define __UINT32_C(value) value##U
#endif
#endif
