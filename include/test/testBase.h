#ifndef TESTBASE_H

#ifdef __cplusplus
extern "C" {
# endif        /* __cplusplus */

#include <stddef.h>

extern void *test_aligned_alloc(size_t align, size_t size);
extern void test_aligned_free(void *ptr);

#ifdef __cplusplus
}
# endif        /* __cplusplus */

#endif /* TESTBASE_H */
