#include "slab/slab.h"
#include "slab/ilist.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>

#define CACHE_LINE_SIZE 64
#define MAXIMUM_ALIGNOF 16

/*
 * The above macros will not work with types wider than uintptr_t, like with
 * uint64 on 32-bit platforms.  That's not problem for the usual use where a
 * pointer or a length is aligned, but for the odd case that you need to
 * align something (potentially) wider, use TYPEALIGN64.
 */
#define TYPEALIGN64(ALIGNVAL, LEN) \
	(((uint64_t) (LEN) + ((ALIGNVAL) -1)) & ~((uint64_t) ((ALIGNVAL) -1)))

/* we don't currently need wider versions of the other ALIGN macros */
#define MAXALIGN(LEN) TYPEALIGN64(MAXIMUM_ALIGNOF, (LEN))


typedef struct
{
	int pagesize;
	int blocksize;
	int block_count;
} slab_info_t;

#define FLEXIBLE_ARRAY_MEMBER
typedef struct
{
	int		   alloc_block_count;
	int		   next_free_index;
	slist_head freelist;
	slab_t	   *slab;
	dlist_node list_node;
} __attribute__((aligned(MAXIMUM_ALIGNOF))) slab_page_t;

static_assert(sizeof(slab_page_t) <= CACHE_LINE_SIZE,
			  "Slab Page header size greater than CACHE_LINE_SIZE bytes");

typedef struct
{
	slist_node next;
} free_block_t;

struct slab_t
{
	slab_info_t slab_info;
	slab_page_t *active_page;

	dlist_head partially_full_pages;
	dlist_head full_pages;

	aligned_alloc_t alloc;
	free_t			free;
	void			*arg_alloc;

	unsigned page_count;
} __attribute__((aligned(CACHE_LINE_SIZE)));

static void		   slab_page_init(slab_page_t *slab_page, slab_t *slab);
static void		   *slab_page_alloc(slab_page_t *page);
static void		   slab_page_free(slab_page_t *page, void *ptr);
static bool		   slab_page_is_empty(slab_page_t *page);
static bool		   slab_page_is_full(slab_page_t *slab_page);
static slab_page_t *slab_alloc_page(slab_t *slab);
static void		   slab_free_page(slab_page_t *slab_page, slab_t *slab);
static void		   *get_user_pointer(void *ptr, slab_page_t *slab_page);
static void		   *get_block_start(void *ptr);
static slab_page_t *get_slab_page(void *ptr);
static void		   *slab_alloc_from_active_page(slab_t *slab);

int
slab_get_header_size(void)
{
	return sizeof(slab_page_t * *);
}


int
slab_control_block_size(void)
{
	return sizeof(slab_t);
}


slab_t *
slab_create(int pagesize, int blocksize, aligned_alloc_t alloc, free_t free, void *arg_alloc)
{
	slab_t *slab = alloc(sizeof(slab_t), CACHE_LINE_SIZE, arg_alloc);

	blocksize = MAXALIGN(blocksize);
	pagesize  = MAXALIGN(pagesize);

	slab->active_page			= NULL;
	slab->slab_info.blocksize	= blocksize;
	slab->slab_info.pagesize	= pagesize;
	slab->slab_info.block_count = (pagesize - sizeof(slab_page_t)) / blocksize;
	slab->arg_alloc				= arg_alloc;
	slab->alloc					= alloc;
	slab->free					= free;
	slab->page_count			= 0;

	dlist_init(&slab->full_pages);
	dlist_init(&slab->partially_full_pages);

	return slab;
}


void
slab_destroy(slab_t *slab)
{
	dlist_mutable_iter iter;

	if (slab->active_page)
	{
		slab_free_page(slab->active_page, slab);
	}

	dlist_foreach_modify(iter, &slab->partially_full_pages)
	{
		slab_page_t *slab_page = dlist_container(slab_page_t, list_node, iter.cur);

		dlist_delete(iter.cur);
		slab_free_page(slab_page, slab);
	}

	dlist_foreach_modify(iter, &slab->full_pages)
	{
		slab_page_t *slab_page = dlist_container(slab_page_t, list_node, iter.cur);

		dlist_delete(iter.cur);
		slab_free_page(slab_page, slab);
	}

	slab->free(slab, slab->arg_alloc);
}


void *
slab_alloc(slab_t *slab)
{
	void *mem = slab_alloc_from_active_page(slab);

	if (mem)
	{
		return mem;
	}

	if (slab->active_page)
	{
		assert(slab_page_is_full(slab->active_page));
		dlist_push_head(&slab->full_pages, &slab->active_page->list_node);
	}

	slab->active_page = NULL;

	if (!dlist_is_empty(&slab->partially_full_pages))
	{
		slab->active_page = dlist_head_element(slab_page_t, list_node, &slab->partially_full_pages);
		dlist_pop_head_node(&slab->partially_full_pages);

		assert(!slab_page_is_empty(slab->active_page));

		return slab_alloc_from_active_page(slab);
	}

	slab->active_page = slab_alloc_page(slab);

	if (slab->active_page)
	{
		return slab_alloc_from_active_page(slab);
	}

	return NULL;
}


void
slab_free(slab_t *slab, void *ptr)
{
	assert(slab != NULL);

	slab_page_t *slab_page	  = get_slab_page(ptr);
	bool		page_was_full = slab_page_is_full(slab_page);

	slab_page_free(slab_page, get_block_start(ptr));

	if (slab_page != slab->active_page)
	{
		if (slab_page_is_empty(slab_page))
		{
			dlist_delete(&slab_page->list_node);
			slab_free_page(slab_page, slab);
		}
		else
		{
			if (page_was_full)
			{
				dlist_delete(&slab_page->list_node);
				dlist_push_head(&slab->partially_full_pages, &slab_page->list_node);
			}
		}
	}
}


size_t
slab_get_size(slab_t *slab)
{
	return slab->page_count * slab->slab_info.pagesize;
}


int
slab_get_page_size(slab_t *slab)
{
	return slab->slab_info.pagesize;
}


static void *
slab_alloc_from_active_page(slab_t *slab)
{
	if (slab->active_page)
	{
		void *mem = slab_page_alloc(slab->active_page);

		if (mem)
		{
			return get_user_pointer(mem, slab->active_page);
		}
	}

	return NULL;
}


static void *
get_user_pointer(void *ptr, slab_page_t *slab_page)
{
	*((slab_page_t **) ptr) = slab_page;
	return (void *) ((char *) ptr + slab_get_header_size());
}


static void *
get_block_start(void *ptr)
{
	return (char *) ptr - slab_get_header_size();
}


static slab_page_t *
get_slab_page(void *ptr)
{
	return *(slab_page_t **) ((char *) ptr - slab_get_header_size());
}


static slab_page_t *
slab_alloc_page(slab_t *slab)
{
	slab_page_t *slab_page = slab->alloc(slab->slab_info.pagesize, CACHE_LINE_SIZE,
										 slab->arg_alloc);

	if (slab_page)
	{
		slab->page_count++;
		slab_page_init(slab_page, slab);
	}

	return slab_page;
}


static void
slab_free_page(slab_page_t *slab_page, slab_t *slab)
{
	slab->page_count--;
	slab->free(slab_page, slab->arg_alloc);
}


static void
slab_page_init(slab_page_t *slab_page, slab_t *slab)
{
	assert(slab_page != NULL);

	slab_page->next_free_index	 = 0;
	slab_page->alloc_block_count = 0;
	slab_page->slab				 = slab;

	slist_init(&slab_page->freelist);
}


static void *
slab_page_alloc(slab_page_t *slab_page)
{
	void		*mem;
	slab_info_t *sinfo;

	assert(slab_page != NULL);

	sinfo = &slab_page->slab->slab_info;

	if (!slist_is_empty(&slab_page->freelist))
	{
		free_block_t *free_block = slist_head_element(free_block_t, next, &slab_page->freelist);

		slist_pop_head_node(&slab_page->freelist);

		slab_page->alloc_block_count++;
		return (void *) free_block;
	}

	if (slab_page->next_free_index < sinfo->block_count)
	{
		char *mem;

		mem = (char *) slab_page + sizeof(slab_page_t) + sinfo->blocksize *
			  (slab_page->next_free_index);

		slab_page->next_free_index++;
		slab_page->alloc_block_count++;
		return mem;
	}

	return NULL;
}


static void
slab_page_free(slab_page_t *slab_page, void *ptr)
{
	assert(slab_page != NULL);

	slab_page->alloc_block_count--;
	slist_push_head(&slab_page->freelist, &((free_block_t *) ptr)->next);
}


static bool
slab_page_is_empty(slab_page_t *slab_page)
{
	assert(slab_page != NULL);

	return slab_page->alloc_block_count == 0;
}


static bool
slab_page_is_full(slab_page_t *slab_page)
{
	assert(slab_page != NULL);

	return slab_page->alloc_block_count == slab_page->slab->slab_info.block_count;
}
