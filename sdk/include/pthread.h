#ifndef SOS2_PTHREAD_H
#define SOS2_PTHREAD_H
#include_next <pthread.h>
#ifdef __cplusplus
extern "C" {
#endif
int pthread_getattr_np(pthread_t, pthread_attr_t *);
/* Native register capture for the engine's stop-the-world collector. */
int sos2_thread_suspend(pthread_t);
int sos2_thread_resume(pthread_t);
struct user_thread_context;
int sos2_thread_context(pthread_t, struct user_thread_context *);
#ifdef __cplusplus
}
#endif
#endif
