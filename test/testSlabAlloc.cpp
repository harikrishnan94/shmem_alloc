#include "test/catch.hpp"
#include "test/testBase.h"
#include "slab/slab.h"

#include <cstdlib>
#include <random>
#include <iostream>
#include <unordered_set>

static void *
slab_base_alloc(size_t size, size_t align, void *arg)
{
	(void) align;
	(void) arg;
	return malloc(size);
}


static void
slab_base_free(void *ptr, void *arg)
{
	(void) arg;
	free(ptr);
}


using random_gen = std::ranlux24_base;

TEST_CASE("SlabAllocatorTest", "[allocator]")
{
	using namespace std;

	constexpr uint32_t SlabAllocSize = 64;
	constexpr uint32_t SlabPageSize	 = 4 * 1024;

	std::random_device r;
	std::seed_seq	   seed{ r(), r(), r(), r(), r(), r(), r(), r() };
	random_gen		   rand_op(seed);

	unordered_set<void *> ptr_set;

	constexpr int blocksize = 1023.94 * 1024;
	constexpr int pagesize	= blocksize * 10;

	slab_t *slab = slab_create(pagesize, blocksize, slab_base_alloc, slab_base_free, NULL);

	constexpr int Low = 1, High = 100;

	std::uniform_int_distribution<int> uniform_dist(Low, High);
	enum
	{
		ALLOC,
		FREE,
		REMOTE_FREE
	};

	auto alloc_type = [](auto val)
					  {
						  if (val > 30)
						  {
							  return ALLOC;
						  }
						  else
						  {
							  return FREE;
						  }
					  };
	auto gen_op = [&rand_op, &uniform_dist]()
				  {
					  return uniform_dist(rand_op);
				  };

	int testIterations = 1024 * 1024;

	for (int i = 0; i < testIterations; i++)
	{
		switch (alloc_type(gen_op()))
		{
			case ALLOC:
			{
				auto mem = slab_alloc(slab);

				if (mem)
				{
					auto usable_memory = SlabAllocSize - slab_get_header_size();

					memset(mem, 0x7F, usable_memory);
				}

				REQUIRE(mem != nullptr);
				REQUIRE(ptr_set.count(mem) == 0);
				ptr_set.insert(mem);
				break;
			}

			case FREE:
			{
				auto iter = ptr_set.begin();

				if (iter != ptr_set.end())
				{
					auto mem = *iter;

					ptr_set.erase(mem);
					slab_free(slab, mem);
				}

				break;
			}

			case REMOTE_FREE:
			{
				auto iter = ptr_set.begin();

				if (iter != ptr_set.end())
				{
					auto mem = *iter;

					ptr_set.erase(mem);
					slab_free(slab, mem);
				}
			}
		}
	}

	for (auto ptr : ptr_set)
	{
		slab_free(slab, ptr);
	}

	REQUIRE(slab_get_size(slab) == slab_get_page_size(slab));
	slab_destroy(slab);
}
