#ifndef SLOCK_H
#define SLOCK_H

#include "spindelay.h"

#include <stdatomic.h>
#include <stdbool.h>

#define DEFAULT_SPIN_DELAY 1000
#define CONSTANT_SPIN_DELAY_BACKOFF 1
#define EXPONENTIAL_SPIN_DELAY_BACKOFF 1

#define SLOCK_LOCKED true
#define SLOCK_UNLOCKED false

typedef struct
{
    _Atomic bool lock;
} slock_t;

static inline void slock_init(slock_t *lock)
{
    atomic_init(&lock->lock, SLOCK_UNLOCKED);
}

static inline bool slock_try_lock(slock_t *lock)
{
    bool old_val = SLOCK_UNLOCKED;
    return atomic_compare_exchange_weak(&lock->lock, &old_val, SLOCK_LOCKED);
}

static inline void slock_lock(slock_t *lock)
{
    while (!slock_try_lock(lock))
    {
        spin_delay_t sds;

        init_spindelay(&sds, DEFAULT_SPIN_DELAY, EXPONENTIAL_SPIN_DELAY_BACKOFF);

        while (atomic_load(&lock->lock))
            perform_spindelay(&sds);
    }
}

static inline void slock_unlock(slock_t *lock)
{
    atomic_store(&lock->lock, SLOCK_UNLOCKED);
}

#endif /* SLOCK_H */
