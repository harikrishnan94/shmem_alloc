#ifndef BMGR_H
#define BMGR_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h>

struct bmgr_t;

typedef struct bmgr_t bmgr_t;

extern bmgr_t *bmgr_create(size_t min_alloc_size, size_t max_alloc_size, void *memory_region, size_t
						   mem_size);
extern void *buddy_alloc(bmgr_t *bmgr, size_t size);
extern void buddy_free(bmgr_t *bmgr, void *ptr, size_t size);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* BMGR_H */
