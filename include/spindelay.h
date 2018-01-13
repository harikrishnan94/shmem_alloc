#ifndef SPINDELAY_H
#define SPINDELAY_H

#include <inttypes.h>
#include <immintrin.h>

typedef struct
{
    uint64_t current_delay;
    uint64_t max_delay;
    uint64_t delay_step;
} spin_delay_t;

#define Min(a, b) (((a) < (b)) ? (a) : (b))

static inline void init_spindelay(spin_delay_t *sds, uint64_t max_delay, uint64_t delay_step)
{
    sds->current_delay = 0;
    sds->max_delay = max_delay;
    sds->delay_step = delay_step;
}

static inline void perform_spindelay(spin_delay_t *sds)
{
    uint64_t current_delay = sds->current_delay;
    sds->current_delay = Min(current_delay * sds->delay_step, sds->max_delay);

    for (uint64_t i = 0; i < current_delay; i++)
        _mm_pause();
}

#endif /* SPINDELAY_H */
