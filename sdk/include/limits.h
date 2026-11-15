/* Newlib leaves the C99 long long limits to GCC's limits.h, which defines
 * them for C99 and later but not for C++11 and later, which requires them
 * too; libstdc++ adds them only in <climits>. */
#include_next <limits.h>
#if defined(__cplusplus) && !defined(LLONG_MAX)
#define LLONG_MIN  (-__LONG_LONG_MAX__ - 1LL)
#define LLONG_MAX  __LONG_LONG_MAX__
#define ULLONG_MAX (__LONG_LONG_MAX__ * 2ULL + 1ULL)
#endif
