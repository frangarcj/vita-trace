#pragma once

#include <pthread.h>
#include <stdlib.h>

/* Owners use PTHREAD_MUTEX_INITIALIZER rather than a C++ global constructor.
 * The mutex stays alive through atexit and Tracy worker teardown. A failed
 * lock must never silently turn a protected operation into an unlocked one. */
class VitaTracyLockGuard {
public:
    explicit VitaTracyLockGuard(pthread_mutex_t *mutex) : mutex_(mutex) {
        if (pthread_mutex_lock(mutex_) != 0) abort();
    }

    ~VitaTracyLockGuard() {
        if (pthread_mutex_unlock(mutex_) != 0) abort();
    }

    VitaTracyLockGuard(const VitaTracyLockGuard &) = delete;
    VitaTracyLockGuard &operator=(const VitaTracyLockGuard &) = delete;

private:
    pthread_mutex_t *mutex_;
};
