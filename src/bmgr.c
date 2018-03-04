#include "bmgr/bmgr.h"
#include "utils/ilist.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef __LINUX__
#define leading_zeroes(x) __builtin_clzl(x)
#endif /* __LINUX__ */

#ifdef __APPLE__
#define leading_zeroes(x) __builtin_clzl(x)
#endif /* __APPLE__ */

#ifdef _WIN32
#include <intrin.h>
#define leading_zeroes(x) __lzcnt64(x)
#endif /* _WIN32 */

typedef void *control_block_t;

#define BITS_PER_BYTE	 8
#define BYTES_PER_QWORD	 8
#define MAX_SIZE_CLASSES (BYTES_PER_QWORD * BITS_PER_BYTE)

#define PTR_TO_NODE(ptr)  ((dlist_node *) ptr)
#define NODE_TO_PTR(node) ((void *) node)

#define Min(a, b) ((uintptr_t) (a) < (uintptr_t) (b) ? (a) : (b))

/* Core struct that contains information about buddy manager */
struct bmgr_t
{
	/* Description of user provided memory region */
	void   *memory_region;
	size_t total_memory_managed;

	/* Start of buddy control blocks */
	control_block_t control_block;
	size_t			control_block_size;

	size_t min_alloc_size;
	size_t max_alloc_size;
	int	   log2_min_alloc_size;
	int	   log2_max_alloc_size;

	void	   *chunk_start;      /* start of allocatable memory region */
	size_t	   num_usable_chunks; /* max chunks that can be allocated */
	size_t	   num_chunks_used;   /* # of chunks that are currently allocated */
	size_t	   next_chunk_index;  /* Index of next free chunk in memory region */
	dlist_head free_chunks;       /* Freelist chunks */

	int		   num_size_classes;                   /* Max size classes */
	dlist_head chunk_free_lists[MAX_SIZE_CLASSES]; /* Freelist of blocks per size class */
};

/* Information needed to manipulate buddy control block for a given pointer */
typedef struct
{
	size_t chunk_id;     /* Id of chunk containing the pointer */
	size_t chunk_offset; /* Offset of pointer within the chunk */
	bmgr_t *bmgr;        /* Buddy manger containing the pointer */
	int	   szc;          /* sizeclass of this pointer */
} buddy_ptr_t;

static int	  log_2(size_t n);
static int	  get_num_size_classes(size_t min_alloc_size, size_t max_alloc_size);
static size_t get_control_block_size(size_t min_alloc_size, size_t max_alloc_size);

static void *chunk_alloc(bmgr_t *bmgr);
static void chunk_free(bmgr_t *bmgr, void *ptr);

static void *buddy_alloc_internal(bmgr_t *bmgr, int szc);
static void buddy_free_internal(bmgr_t *bmgr, void *ptr, int szc);
static void adjust_control_block(bmgr_t *bmgr, void *ptr, int szc, bool split);

static buddy_ptr_t get_buddy_ptr(bmgr_t *bmgr, void *ptr, int szc);
static void		   *get_real_ptr(bmgr_t *bmgr, buddy_ptr_t bptr);

static int	  get_size_class(bmgr_t *bmgr, size_t size);
static size_t get_size(bmgr_t *bmgr, int szc);

static control_block_t get_control_block(buddy_ptr_t bptr);
static size_t		   get_bitmap_index(buddy_ptr_t bptr);
static void			   mark_as_in_use(control_block_t control_block, buddy_ptr_t bptr);
static void			   mark_as_free(control_block_t control_block, buddy_ptr_t bptr);
static bool			   block_is_free(control_block_t control_block, buddy_ptr_t bptr);
static bool			   both_free(control_block_t control_block, buddy_ptr_t bptr);
static buddy_ptr_t	   get_buddy(control_block_t control_block, buddy_ptr_t bptr);

#define TOGGLE_BIT(val, bit) (val ^ (1 << bit))

bmgr_t *
bmgr_create(size_t min_alloc_size, size_t max_alloc_size, void *memory_region, size_t mem_size)
{
	bmgr_t	  *bmgr = memory_region;
	size_t	  max_usable_chunks;
	ptrdiff_t usable_mem_size;
	size_t	  mem_used;

	assert(bmgr != NULL);

	max_usable_chunks = mem_size / max_alloc_size;
	mem_used		  = sizeof(bmgr_t);

	if (mem_size <= mem_used)
	{
		return NULL;
	}

	bmgr->memory_region		   = memory_region;
	bmgr->total_memory_managed = mem_size;
	bmgr->min_alloc_size	   = min_alloc_size;
	bmgr->max_alloc_size	   = max_alloc_size;
	bmgr->log2_min_alloc_size  = log_2(min_alloc_size);
	bmgr->log2_max_alloc_size  = log_2(max_alloc_size);
	bmgr->num_chunks_used	   = 0;
	bmgr->next_chunk_index	   = 0;
	bmgr->num_size_classes	   = get_num_size_classes(min_alloc_size, max_alloc_size);
	bmgr->control_block_size   = get_control_block_size(min_alloc_size, max_alloc_size);
	bmgr->control_block		   = (control_block_t) ((char *) memory_region + sizeof(bmgr_t));
	mem_used				  += bmgr->control_block_size * max_usable_chunks;

	if (bmgr->control_block_size == 0 || mem_size <= mem_used)
	{
		return NULL;
	}

	/* Align chunk_start to max_alloc_size, so that start of chunk is efficiently computed via bit manipulations */
	bmgr->chunk_start = (void *) TYPEALIGN64(max_alloc_size, (char *) bmgr->control_block +
											 max_usable_chunks * bmgr->control_block_size);

	for (int i = 0; i < bmgr->num_size_classes; i++)
	{
		dlist_init(&bmgr->chunk_free_lists[i]);
	}

	/* Size of alloca'ble memory region */
	usable_mem_size = ((char *) memory_region + mem_size) - (char *) bmgr->chunk_start;

	if (usable_mem_size > 0)
	{
		bmgr->num_usable_chunks = usable_mem_size / max_alloc_size;
	}

	memset(bmgr->control_block, 0, bmgr->num_usable_chunks * bmgr->control_block_size);

	dlist_init(&bmgr->free_chunks);

	return bmgr;
}


size_t
buddy_total_alloc_memory(bmgr_t *bmgr)
{
	return bmgr->num_usable_chunks * bmgr->max_alloc_size;
}


/* Allocate memory region of size 'size' */
void *
buddy_alloc(bmgr_t *bmgr, size_t size)
{
	if (size < bmgr->min_alloc_size || size > bmgr->max_alloc_size)
	{
		return NULL;
	}

	return buddy_alloc_internal(bmgr, get_size_class(bmgr, size));
}


/* Free memory region pointed by ptr of size 'size' */
void
buddy_free(bmgr_t *bmgr, void *ptr, size_t size)
{
	if (ptr < bmgr->chunk_start || ptr > bmgr->memory_region + bmgr->total_memory_managed)
	{
		fprintf(stderr, "bmgr: Freeing invalid pointer");
		abort();
	}

	if (size < bmgr->min_alloc_size || size > bmgr->max_alloc_size)
	{
		return;
	}

	assert(((uintptr_t) ptr) % bmgr->min_alloc_size == 0);

	buddy_free_internal(bmgr, ptr, get_size_class(bmgr, size));
}


/*
 * Core buddy alloc algorithm:
 *  If there is a free block available for the target size class
 *      Retun the block.
 *  Else If the target size class is not maximum size class,
 *      Allocate a block from next size class and split that into two and
 *      return one block and move it's buddy to current sizeclass's freelist.
 * Else (For max size class)
 *      Allocate a full chunk and return it.
 */
static void *
buddy_alloc_internal(bmgr_t *bmgr, int szc)
{
	void *ptr;
	bool split;

	assert(szc >= 0);

	if (!dlist_is_empty(&bmgr->chunk_free_lists[szc]))
	{
		ptr	  = NODE_TO_PTR(dlist_pop_head_node(&bmgr->chunk_free_lists[szc]));
		split = false;
	}
	else if (szc != bmgr->num_size_classes - 1)
	{
		ptr	  = buddy_alloc_internal(bmgr, szc + 1);
		split = true;
	}
	else
	{
		ptr	  = chunk_alloc(bmgr);
		split = false;
	}

	if (ptr)
	{
		adjust_control_block(bmgr, ptr, szc, split);
	}

	return ptr;
}


/*
 * Core buddy free algorithm:
 *	If size class is maximum size class, then
 *		Free the entire chunk (block)
 *  Else If the buddy of current free block is also free, then
 *      Merge two blocks and free the merged block.
 *	Else
 *		Push the block into freelist of current sizeclass
 */
static void
buddy_free_internal(bmgr_t *bmgr, void *ptr, int szc)
{
	assert(szc >= 0);
	assert(ptr != NULL);

	buddy_ptr_t		bptr		  = get_buddy_ptr(bmgr, ptr, szc);
	control_block_t control_block = get_control_block(bptr);

	assert(!both_free(control_block, bptr));

	mark_as_free(control_block, bptr);

	if (szc != bmgr->num_size_classes - 1)
	{
		buddy_ptr_t buddy_bptr = get_buddy(control_block, bptr);

		if (block_is_free(control_block, buddy_bptr))
		{
			void *buddy = get_real_ptr(bmgr, buddy_bptr);

			dlist_delete(PTR_TO_NODE(buddy));
			buddy_free_internal(bmgr, Min(ptr, buddy), szc + 1);
		}
		else
		{
			dlist_push_head(&bmgr->chunk_free_lists[szc], PTR_TO_NODE(ptr));
		}
	}
	else
	{
		chunk_free(bmgr, ptr);
	}
}


/*
 * Adjust control block for the given pointer.
 */
static void
adjust_control_block(bmgr_t *bmgr, void *ptr, int szc, bool split)
{
	buddy_ptr_t		bptr		  = get_buddy_ptr(bmgr, ptr, szc);
	control_block_t control_block = get_control_block(bptr);

	mark_as_in_use(control_block, bptr);

	/* If the block needs to be split, then push it's buddy into freelist */
	if (split)
	{
		buddy_ptr_t buddy_bptr = get_buddy(control_block, bptr);
		void		*buddy	   = get_real_ptr(bmgr, buddy_bptr);

		assert(block_is_free(control_block, buddy_bptr));

		dlist_push_head(&bmgr->chunk_free_lists[szc], PTR_TO_NODE(buddy));
	}
}


static int
get_num_size_classes(size_t min_alloc_size, size_t max_alloc_size)
{
	if (min_alloc_size < 16 || max_alloc_size <= min_alloc_size)
	{
		return 0;
	}

	int log2_min = log_2(min_alloc_size);
	int log2_max = log_2(max_alloc_size);

	/* Check if max_alloc_size and min_alloc_size are power of two */
	if (((1 << log2_max) != max_alloc_size) || ((1 << log2_min) != min_alloc_size))
	{
		return 0;
	}

	return log2_max - log2_min + 1;
}


static size_t
get_control_block_size(size_t min_alloc_size, size_t max_alloc_size)
{
	return MAXALIGN((max_alloc_size / min_alloc_size * 2) / 8);
}


/* Allocate a chunk from managed memory region */
static void *
chunk_alloc(bmgr_t *bmgr)
{
	void *chunk = NULL;

	/* check if there is free chunk available */
	if (!dlist_is_empty(&bmgr->free_chunks))
	{
		bmgr->num_chunks_used++;
		chunk = NODE_TO_PTR(dlist_pop_head_node(&bmgr->free_chunks));
	}
	else if (bmgr->next_chunk_index < bmgr->num_usable_chunks)
	{
		/* Allocate next chunk, from the memory region, If all chunks are not in use */
		chunk = (char *) bmgr->chunk_start + bmgr->next_chunk_index * bmgr->max_alloc_size;

		bmgr->next_chunk_index++;
		bmgr->num_chunks_used++;
	}

#ifdef USE_ASSERT_CHECKING

	if (chunk)
	{
		buddy_ptr_t		bptr		  = get_buddy_ptr(bmgr, chunk, bmgr->num_size_classes - 1);
		control_block_t control_block = get_control_block(bptr);

		for (size_t i = 0; i < bmgr->control_block_size; i++)
		{
			assert(((uint8_t *) control_block)[i] == 0);
		}
	}

#endif /* USE_ASSERT_CHECKING */

	return chunk;
}


static void
chunk_free(bmgr_t *bmgr, void *ptr)
{
	assert(ptr != NULL);
	assert(((uintptr_t) ptr) % bmgr->max_alloc_size == 0);

	bmgr->num_chunks_used--;
	dlist_push_head(&bmgr->free_chunks, PTR_TO_NODE(ptr));
}


static buddy_ptr_t
get_buddy_ptr(bmgr_t *bmgr, void *ptr, int szc)
{
	buddy_ptr_t bptr;
	uintptr_t	ptr_val			= (uintptr_t) ptr;
	uintptr_t	chunk_start_val = (uintptr_t) bmgr->chunk_start;

	assert(chunk_start_val <= ptr_val);

	bptr.chunk_id	  = (ptr_val - chunk_start_val) / bmgr->max_alloc_size;
	bptr.chunk_offset = (ptr_val - chunk_start_val) % bmgr->max_alloc_size;
	bptr.szc		  = szc;
	bptr.bmgr		  = bmgr;

	return bptr;
}


static void *
get_real_ptr(bmgr_t *bmgr, buddy_ptr_t bptr)
{
	return (void *) (bptr.chunk_id * bmgr->max_alloc_size + (uintptr_t) bmgr->chunk_start +
					 bptr.chunk_offset);
}


static int
get_size_class(bmgr_t *bmgr, size_t size)
{
	return log_2(size) - bmgr->log2_min_alloc_size;
}


static size_t
get_size(bmgr_t *bmgr, int szc)
{
	return bmgr->min_alloc_size * (((size_t) 1) << szc);
}


static control_block_t
get_control_block(buddy_ptr_t bptr)
{
	bmgr_t *bmgr = bptr.bmgr;

	return (control_block_t) ((char *) bmgr->control_block + bptr.chunk_id *
							  bmgr->control_block_size);
}


static size_t
get_bitmap_index(buddy_ptr_t bptr)
{
	bmgr_t *bmgr = bptr.bmgr;

	return (1 << (bmgr->num_size_classes - (bptr.szc + 1))) - 1 + bptr.chunk_offset /
		   get_size(bmgr, bptr.szc);
}


static void
mark_as_in_use(control_block_t control_block, buddy_ptr_t bptr)
{
	uint8_t *bitmap		 = control_block;
	size_t	bitmap_index = get_bitmap_index(bptr);
	size_t	bitmap_byte	 = bitmap_index / 8;
	size_t	bitmap_bit	 = bitmap_index % 8;

	assert(block_is_free(control_block, bptr));

	bitmap[bitmap_byte] |= 1 << bitmap_bit;
}


static void
mark_as_free(control_block_t control_block, buddy_ptr_t bptr)
{
	uint8_t *bitmap		 = control_block;
	size_t	bitmap_index = get_bitmap_index(bptr);
	size_t	bitmap_byte	 = bitmap_index / 8;
	size_t	bitmap_bit	 = bitmap_index % 8;

	assert(!block_is_free(control_block, bptr));

	bitmap[bitmap_byte] &= ~(1 << bitmap_bit);
}


static bool
block_is_free(control_block_t control_block, buddy_ptr_t bptr)
{
	uint8_t *bitmap		 = control_block;
	size_t	bitmap_index = get_bitmap_index(bptr);
	size_t	bitmap_byte	 = bitmap_index / 8;
	size_t	bitmap_bit	 = bitmap_index % 8;

	return (bitmap[bitmap_byte] & (1 << bitmap_bit)) == false;
}


static bool
both_free(control_block_t control_block, buddy_ptr_t bptr)
{
	return block_is_free(control_block, bptr) && block_is_free(control_block, get_buddy(
																   control_block, bptr));
}


static buddy_ptr_t
get_buddy(control_block_t control_block, buddy_ptr_t bptr)
{
	size_t size		 = get_size(bptr.bmgr, bptr.szc);
	int	   log2_size = log_2(size);
	size_t n_ptr	 = bptr.chunk_offset >> log2_size;

	bptr.chunk_offset = (n_ptr ^ 0x1) << log2_size;

	return bptr;
}


static int
log_2(size_t n)
{
	assert(n > 0);

	int lz = leading_zeroes(n);

	return sizeof(n) * BITS_PER_BYTE - lz - 1;
}
