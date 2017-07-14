/*
  Copyright (c) 2014 Cisco and/or its affiliates.

  * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/** @cond DOCUMENTATION_IS_IN_BIHASH_DOC_H */

/*
 * Note: to instantiate the template multiple times in a single file,
 * #undef __included_bihash_template_h__...
 */
#ifndef __included_bihash_template_h__
#define __included_bihash_template_h__

#include <vppinfra/heap.h>
#include <vppinfra/format.h>
#include <vppinfra/pool.h>

#ifndef BIHASH_TYPE
#error BIHASH_TYPE not defined
#endif

#define _bv(a,b) a##b
#define __bv(a,b) _bv(a,b)
#define BV(a) __bv(a,BIHASH_TYPE)

#define _bvt(a,b) a##b##_t
#define __bvt(a,b) _bvt(a,b)
#define BVT(a) __bvt(a,BIHASH_TYPE)

typedef struct BV (clib_bihash_value)
{
  union
  {
    BVT (clib_bihash_kv) kvp[BIHASH_KVP_PER_PAGE];
    struct BV (clib_bihash_value) * next_free;
  };
} BVT (clib_bihash_value);

#if BIHASH_KVP_CACHE_SIZE > 5
#error Requested KVP cache LRU data exceeds 16 bits
#endif

typedef struct
{
  union
  {
    struct
    {
      u32 offset;
      u8 linear_search;
      u8 log2_pages;
      u16 cache_lru;
    };
    u64 as_u64;
  };
    BVT (clib_bihash_kv) cache[BIHASH_KVP_CACHE_SIZE];
} BVT (clib_bihash_bucket);

typedef struct
{
  BVT (clib_bihash_value) * values;
  BVT (clib_bihash_bucket) * buckets;
  volatile u32 *writer_lock;

    BVT (clib_bihash_value) ** working_copies;
  int *working_copy_lengths;
    BVT (clib_bihash_bucket) saved_bucket;

  u32 nbuckets;
  u32 log2_nbuckets;
  u32 linear_buckets;
  u8 *name;

  u64 cache_hits;
  u64 cache_misses;

    BVT (clib_bihash_value) ** freelists;
  void *mheap;

} BVT (clib_bihash);


static inline void
BV (clib_bihash_update_lru) (BVT (clib_bihash_bucket) * b, u8 slot)
{
  u16 value, tmp, mask;
  u8 found_lru_pos;
  u16 save_hi;

  if (BIHASH_KVP_CACHE_SIZE < 2)
    return;

  ASSERT (slot < BIHASH_KVP_CACHE_SIZE);

  /* First, find the slot in cache_lru */
  mask = slot;
  if (BIHASH_KVP_CACHE_SIZE > 1)
    mask |= slot << 3;
  if (BIHASH_KVP_CACHE_SIZE > 2)
    mask |= slot << 6;
  if (BIHASH_KVP_CACHE_SIZE > 3)
    mask |= slot << 9;
  if (BIHASH_KVP_CACHE_SIZE > 4)
    mask |= slot << 12;

  value = b->cache_lru;
  tmp = value ^ mask;

  /* Already the most-recently used? */
  if ((tmp & 7) == 0)
    return;

  found_lru_pos = ((tmp & (7 << 3)) == 0) ? 1 : 0;
  if (BIHASH_KVP_CACHE_SIZE > 2)
    found_lru_pos = ((tmp & (7 << 6)) == 0) ? 2 : found_lru_pos;
  if (BIHASH_KVP_CACHE_SIZE > 3)
    found_lru_pos = ((tmp & (7 << 9)) == 0) ? 3 : found_lru_pos;
  if (BIHASH_KVP_CACHE_SIZE > 4)
    found_lru_pos = ((tmp & (7 << 12)) == 0) ? 4 : found_lru_pos;

  ASSERT (found_lru_pos);

  /* create a mask to kill bits in or above slot */
  mask = 0xFFFF << found_lru_pos;
  mask <<= found_lru_pos;
  mask <<= found_lru_pos;
  mask ^= 0xFFFF;
  tmp = value & mask;

  /* Save bits above slot */
  mask ^= 0xFFFF;
  mask <<= 3;
  save_hi = value & mask;

  value = save_hi | (tmp << 3) | slot;

  b->cache_lru = value;
}

void
BV (clib_bihash_update_lru_not_inline) (BVT (clib_bihash_bucket) * b,
					u8 slot);

static inline u8 BV (clib_bihash_get_lru) (BVT (clib_bihash_bucket) * b)
{
  return (b->cache_lru >> (3 * (BIHASH_KVP_CACHE_SIZE - 1))) & 7;
}

static inline void BV (clib_bihash_reset_cache) (BVT (clib_bihash_bucket) * b)
{
  u16 initial_lru_value;

  memset (b->cache, 0xff, sizeof (b->cache));

  /*
   * We'll want the cache to be loaded from slot 0 -> slot N, so
   * the initial LRU order is reverse index order.
   */
  if (BIHASH_KVP_CACHE_SIZE == 1)
    initial_lru_value = 0;
  else if (BIHASH_KVP_CACHE_SIZE == 2)
    initial_lru_value = (0 << 3) | (1 << 0);
  else if (BIHASH_KVP_CACHE_SIZE == 3)
    initial_lru_value = (0 << 6) | (1 << 3) | (2 << 0);
  else if (BIHASH_KVP_CACHE_SIZE == 4)
    initial_lru_value = (0 << 9) | (1 << 6) | (2 << 3) | (3 << 0);
  else if (BIHASH_KVP_CACHE_SIZE == 5)
    initial_lru_value = (0 << 12) | (1 << 9) | (2 << 6) | (3 << 3) | (4 << 0);

  b->cache_lru = initial_lru_value;
}

static inline void BV (clib_bihash_cache_enable_disable)
  (BVT (clib_bihash_bucket) * b, u8 enable)
{
  BVT (clib_bihash_bucket) tmp_b;
  tmp_b.as_u64 = b->as_u64;
  tmp_b.cache_lru &= 0x7FFF;
  tmp_b.cache_lru |= enable << 15;
  b->as_u64 = tmp_b.as_u64;
}

static inline void *BV (clib_bihash_get_value) (BVT (clib_bihash) * h,
						uword offset)
{
  u8 *hp = h->mheap;
  u8 *vp = hp + offset;

  return (void *) vp;
}

static inline uword BV (clib_bihash_get_offset) (BVT (clib_bihash) * h,
						 void *v)
{
  u8 *hp, *vp;

  hp = (u8 *) h->mheap;
  vp = (u8 *) v;

  ASSERT ((vp - hp) < 0x100000000ULL);
  return vp - hp;
}

void BV (clib_bihash_init)
  (BVT (clib_bihash) * h, char *name, u32 nbuckets, uword memory_size);

void BV (clib_bihash_free) (BVT (clib_bihash) * h);

int BV (clib_bihash_add_del) (BVT (clib_bihash) * h,
			      BVT (clib_bihash_kv) * add_v, int is_add);
int BV (clib_bihash_search) (BVT (clib_bihash) * h,
			     BVT (clib_bihash_kv) * search_v,
			     BVT (clib_bihash_kv) * return_v);

void BV (clib_bihash_foreach_key_value_pair) (BVT (clib_bihash) * h,
					      void *callback, void *arg);

format_function_t BV (format_bihash);
format_function_t BV (format_bihash_kvp);
format_function_t BV (format_bihash_lru);

static inline int BV (clib_bihash_search_inline)
  (BVT (clib_bihash) * h, BVT (clib_bihash_kv) * key_result)
{
  u64 hash;
  u32 bucket_index;
  BVT (clib_bihash_value) * v;
  BVT (clib_bihash_bucket) * b;
  BVT (clib_bihash_kv) * kvp;
  int i, limit;

  hash = BV (clib_bihash_hash) (key_result);

  bucket_index = hash & (h->nbuckets - 1);
  b = &h->buckets[bucket_index];

  if (b->offset == 0)
    return -1;

  /* Check the cache, if currently enabled */
  if (PREDICT_TRUE (b->cache_lru & (1 << 15)))
    {
      limit = BIHASH_KVP_CACHE_SIZE;
      kvp = b->cache;
      for (i = 0; i < limit; i++)
	{
	  if (BV (clib_bihash_key_compare) (kvp[i].key, key_result->key))
	    {
	      *key_result = kvp[i];
	      h->cache_hits++;
	      return 0;
	    }
	}
    }

  hash >>= h->log2_nbuckets;

  v = BV (clib_bihash_get_value) (h, b->offset);

  /* If the bucket has unresolvable collisions, use linear search */
  limit = BIHASH_KVP_PER_PAGE;
  v += (b->linear_search == 0) ? hash & ((1 << b->log2_pages) - 1) : 0;
  if (PREDICT_FALSE (b->linear_search))
    limit <<= b->log2_pages;

  for (i = 0; i < limit; i++)
    {
      if (BV (clib_bihash_key_compare) (v->kvp[i].key, key_result->key))
	{
	  u8 cache_slot;
	  *key_result = v->kvp[i];

	  /* Shut off the cache */
	  BV (clib_bihash_cache_enable_disable) (b, 0);
	  CLIB_MEMORY_BARRIER ();

	  cache_slot = BV (clib_bihash_get_lru) (b);
	  b->cache[cache_slot] = v->kvp[i];
	  BV (clib_bihash_update_lru) (b, cache_slot);

	  /* Reenable the cache */
	  BV (clib_bihash_cache_enable_disable) (b, 1);
	  h->cache_misses++;
	  return 0;
	}
    }
  return -1;
}

static inline int BV (clib_bihash_search_inline_2)
  (BVT (clib_bihash) * h,
   BVT (clib_bihash_kv) * search_key, BVT (clib_bihash_kv) * valuep)
{
  u64 hash;
  u32 bucket_index;
  BVT (clib_bihash_value) * v;
  BVT (clib_bihash_bucket) * b;
  BVT (clib_bihash_kv) * kvp;
  int i, limit;

  ASSERT (valuep);

  hash = BV (clib_bihash_hash) (search_key);

  bucket_index = hash & (h->nbuckets - 1);
  b = &h->buckets[bucket_index];

  if (b->offset == 0)
    return -1;

  /* Check the cache, if currently enabled */
  if (PREDICT_TRUE (b->cache_lru & (1 << 15)))
    {
      limit = BIHASH_KVP_CACHE_SIZE;
      kvp = b->cache;
      for (i = 0; i < limit; i++)
	{
	  if (BV (clib_bihash_key_compare) (kvp[i].key, search_key->key))
	    {
	      *valuep = kvp[i];
	      h->cache_hits++;
	      return 0;
	    }
	}
    }

  hash >>= h->log2_nbuckets;
  v = BV (clib_bihash_get_value) (h, b->offset);

  /* If the bucket has unresolvable collisions, use linear search */
  limit = BIHASH_KVP_PER_PAGE;
  v += (b->linear_search == 0) ? hash & ((1 << b->log2_pages) - 1) : 0;
  if (PREDICT_FALSE (b->linear_search))
    limit <<= b->log2_pages;

  for (i = 0; i < limit; i++)
    {
      if (BV (clib_bihash_key_compare) (v->kvp[i].key, search_key->key))
	{
	  u8 cache_slot;
	  *valuep = v->kvp[i];

	  /* Shut off the cache */
	  BV (clib_bihash_cache_enable_disable) (b, 0);
	  CLIB_MEMORY_BARRIER ();

	  cache_slot = BV (clib_bihash_get_lru) (b);
	  b->cache[cache_slot] = v->kvp[i];
	  BV (clib_bihash_update_lru) (b, cache_slot);

	  /* Reenable the cache */
	  BV (clib_bihash_cache_enable_disable) (b, 1);
	  h->cache_misses++;
	  return 0;
	}
    }
  return -1;
}

#endif /* __included_bihash_template_h__ */

/** @endcond */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
