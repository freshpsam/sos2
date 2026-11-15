#include <semaphore.h>
#include <errno.h>
#include <string.h>

static int result(int error) {
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int sem_init(sem_t *sem, int shared, unsigned value) {
    if (shared) return result(ENOSYS);
    if (!sem || value > SEM_VALUE_MAX) return result(EINVAL);
    memset(sem, 0, sizeof(*sem));
    int error = pthread_mutex_init(&sem->mutex, NULL);
    if (error) return result(error);
    error = pthread_cond_init(&sem->condition, NULL);
    if (error) {
        pthread_mutex_destroy(&sem->mutex);
        return result(error);
    }
    sem->value = value;
    sem->initialized = 1;
    return 0;
}

int sem_destroy(sem_t *sem) {
    if (!sem || !sem->initialized) return result(EINVAL);
    pthread_mutex_lock(&sem->mutex);
    if (sem->waiters) {
        pthread_mutex_unlock(&sem->mutex);
        return result(EBUSY);
    }
    sem->initialized = 0;
    pthread_mutex_unlock(&sem->mutex);
    int error = pthread_cond_destroy(&sem->condition), other = pthread_mutex_destroy(&sem->mutex);
    return result(error ? error : other);
}

int sem_post(sem_t *sem) {
    if (!sem || !sem->initialized) return result(EINVAL);
    pthread_mutex_lock(&sem->mutex);
    int error = 0;
    if (sem->value == SEM_VALUE_MAX) error = EOVERFLOW;
    else {
        ++sem->value;
        if (sem->waiters) error = pthread_cond_signal(&sem->condition);
    }
    pthread_mutex_unlock(&sem->mutex);
    return result(error);
}

static int wait_for(sem_t *sem, int immediate, const struct timespec *deadline) {
    if (!sem || !sem->initialized) return result(EINVAL);
    if (deadline && (deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000))
        return result(EINVAL);
    pthread_mutex_lock(&sem->mutex);
    int error = 0;
    ++sem->waiters;
    while (!sem->value) {
        if (immediate) {
            error = EAGAIN;
            break;
        }
        error = deadline ? pthread_cond_timedwait(&sem->condition, &sem->mutex, deadline)
                         : pthread_cond_wait(&sem->condition, &sem->mutex);
        if (error) break;
    }
    --sem->waiters;
    if (!error) --sem->value;
    pthread_mutex_unlock(&sem->mutex);
    return result(error);
}

int sem_wait(sem_t *sem) { return wait_for(sem, 0, NULL); }

int sem_trywait(sem_t *sem) { return wait_for(sem, 1, NULL); }

int sem_timedwait(sem_t *sem, const struct timespec *deadline) {
    if (!deadline) return result(EINVAL);
    return wait_for(sem, 0, deadline);
}

int sem_getvalue(sem_t *sem, int *value) {
    if (!sem || !sem->initialized || !value) return result(EINVAL);
    pthread_mutex_lock(&sem->mutex);
    *value = sem->value;
    pthread_mutex_unlock(&sem->mutex);
    return 0;
}
