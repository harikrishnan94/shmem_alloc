#ifndef SLAB_H
#define SLAB_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h>

struct slab_t;

typedef struct slab_t slab_t;
typedef void *(*aligned_alloc_t)(size_t size, size_t align, void *arg);
typedef void (*free_t)(void *ptr, void *arg);

extern int	  slab_control_block_size(void);
extern int	  slab_get_header_size(void);
extern slab_t *slab_create(int pagesize, int blocksize, aligned_alloc_t alloc, free_t free,
						   void *arg_alloc);
extern void slab_destroy(slab_t *slab);

extern void	  *slab_alloc(slab_t *slab);
extern void	  slab_free(slab_t *slab, void *ptr);
extern size_t slab_get_size(slab_t *slab);

#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* SLAB_H */
