#include "test/catch.hpp"
#include "test/testBase.h"
#include "bmgr/bmgr.h"

#include <memory>
#include <cstdlib>
#include <random>
#include <unordered_map>
#include <list>
#include <iostream>

using random_gen = std::ranlux24_base;

static void
randomize_mem(random_gen &rand, void *ptr, size_t size)
{
	auto mem = reinterpret_cast<size_t *>(ptr);

	for (size_t i = 0; i < size / sizeof(size_t); i++)
	{
		mem[i] = rand();
	}
}


TEST_CASE("BuddyManager Test", "[allocator]")
{
	using namespace std;

	std::random_device r;
	std::seed_seq	   seed{ r(), r(), r(), r(), r(), r(), r(), r() };
	random_gen		   rand_mem(seed);

	constexpr auto BuddyPageSize		  = 4 * 1024 * 1024;
	constexpr auto BuddyMinAllocSize	  = 4 * 1024;
	constexpr auto BuddyManagerAllocLimit = 28 * 1024 * 1024;
	constexpr auto AllocLimit			  = BuddyManagerAllocLimit - BuddyPageSize -
											2 * 1024 * 1024;
	constexpr auto MinAllocSize = BuddyMinAllocSize;

	void   *buddy_mem	  = static_cast<void *>(new char[BuddyManagerAllocLimit]);
	bmgr_t *buddy_manager = bmgr_create(BuddyMinAllocSize, BuddyPageSize, buddy_mem,
										BuddyManagerAllocLimit);

	unordered_map<void *, size_t> ptr_set;

	auto do_alloc = [&ptr_set, &buddy_manager, &rand_mem](auto &alloc_size_vector)
					{
						int	   i			 = 0;
						size_t allocated_mem = 0;

						for (auto alloc_size : alloc_size_vector)
						{
							auto mem = buddy_alloc(buddy_manager, alloc_size);

							REQUIRE(mem != nullptr);
							REQUIRE(ptr_set.count(mem) == 0);

							if (mem)
							{
								ptr_set.insert({ mem, alloc_size });
								randomize_mem(rand_mem, mem, alloc_size);
								allocated_mem += alloc_size;
							}

							i++;
						}
					};

	auto do_free = [&ptr_set, &buddy_manager](auto free_size = 0)
				   {
					   vector<pair<void *, size_t> > reclaim_ptrs{ };

					   for (auto &ptr_size_p : ptr_set)
					   {
						   if (free_size == 0 || free_size == ptr_size_p.second)
						   {
							   buddy_free(buddy_manager, ptr_size_p.first, ptr_size_p.second);
							   reclaim_ptrs.push_back({ ptr_size_p.first, ptr_size_p.second });
						   }
					   }

					   for (auto &ptr_size_p : reclaim_ptrs)
					   {
						   ptr_set.erase(ptr_size_p.first);
					   }
				   };

	vector<size_t> alloc_size_vector{ };

	alloc_size_vector.push_back(MinAllocSize);
	alloc_size_vector.push_back(MinAllocSize * 2);
	alloc_size_vector.push_back(MinAllocSize * 4);
	alloc_size_vector.push_back(MinAllocSize * 8);
	alloc_size_vector.push_back(MinAllocSize * 16);
	alloc_size_vector.push_back(BuddyPageSize / 4);

	REQUIRE(buddy_alloc(buddy_manager, BuddyPageSize + 1) == nullptr);

	do_alloc(alloc_size_vector);
	do_free(0);

	alloc_size_vector.clear();
	alloc_size_vector.push_back(BuddyPageSize / 2);
	alloc_size_vector.push_back(BuddyPageSize / 4);
	alloc_size_vector.push_back(BuddyPageSize / 4);
	do_alloc(alloc_size_vector);
	do_free(BuddyPageSize / 2);

	alloc_size_vector.clear();

	for (int i = 0; i < AllocLimit / MinAllocSize; i++)
	{
		alloc_size_vector.push_back(MinAllocSize);
	}

	do_alloc(alloc_size_vector);
	do_free(0);

	alloc_size_vector.clear();
	alloc_size_vector.push_back(BuddyPageSize / 2);
	alloc_size_vector.push_back(BuddyPageSize / 4);
	alloc_size_vector.push_back(BuddyPageSize / 4);
	do_alloc(alloc_size_vector);
}
