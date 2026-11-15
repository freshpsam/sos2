#ifndef SOS2_FEATURES_H
#define SOS2_FEATURES_H
#include <sos2/abi.h>
/* Source-level library interfaces implemented by SOS2, never Linux ABI calls. */
#define __sos2__                        1
#define __DYNAMIC_REENT__               1
#define _GNU_SOURCE                     1
#define _POSIX_THREADS                  1
#define _UNIX98_THREAD_MUTEX_ATTRIBUTES 1
#define _POSIX_TIMERS                   1
#define _POSIX_MONOTONIC_CLOCK          1
#define _POSIX_CLOCK_SELECTION          1
#define _POSIX_READER_WRITER_LOCKS      1
#define _POSIX_BARRIERS                 1
#define _POSIX_TIMEOUTS                 1
/* Native static programs always have threads. Do not probe for an unrelated
 * weak pthread_cancel symbol to decide whether synchronization is active. */
#define GTHREAD_USE_WEAK          0
#define _GLIBCXX_GTHREAD_USE_WEAK 0
/* Newlib pthread mutexes are allocated handles, not inline lock storage.
 * libstdc++ must initialize AND destroy them: its constant-initializer path
 * gives std::mutex a trivial destructor and leaks every temporary handle. */
#define _GTHREAD_USE_MUTEX_INIT_FUNC           1
#define _GTHREAD_USE_RECURSIVE_MUTEX_INIT_FUNC 1
#endif
