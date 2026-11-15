#ifndef SOS2_SEMAPHORE_H
#define SOS2_SEMAPHORE_H
#include <pthread.h>
#include <time.h>
#include <limits.h>
#define SEM_VALUE_MAX INT_MAX

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned value, waiters, initialized;
} sem_t;
#ifdef __cplusplus
extern "C" {
#endif
int sem_init(sem_t *, int, unsigned);
int sem_destroy(sem_t *);
int sem_post(sem_t *);
int sem_wait(sem_t *);
int sem_trywait(sem_t *);
int sem_timedwait(sem_t *, const struct timespec *);
int sem_getvalue(sem_t *, int *);
#ifdef __cplusplus
}
#endif
#endif
