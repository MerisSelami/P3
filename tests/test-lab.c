#include <assert.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "harness/unity.h"
#include "../src/lab.h"

/* ------------------------------------------------------------------
 *  Setup / Teardown
 * ------------------------------------------------------------------ */
void setUp(void) {
  // Called before each test
}

void tearDown(void) {
  // Called after each test
}

/* ------------------------------------------------------------------
 *  Helper checks
 * ------------------------------------------------------------------ */

/**
 * @brief Check the pool to ensure it is "full" — that is, one big free block.
 *        The base block is in pool->avail[pool->kval_m], and all smaller lists
 *        are empty.
 */
static void check_buddy_pool_full(struct buddy_pool *pool)
{
  // For i in [0, kval_m - 1], the lists should be empty
  for (size_t i = 0; i < pool->kval_m; i++) {
    TEST_ASSERT_TRUE(pool->avail[i].next == &pool->avail[i]);
    TEST_ASSERT_TRUE(pool->avail[i].prev == &pool->avail[i]);
    TEST_ASSERT_EQUAL_UINT16(BLOCK_UNUSED, pool->avail[i].tag);
    TEST_ASSERT_EQUAL_UINT16(i, pool->avail[i].kval);
  }

  // The highest list (kval_m) should have exactly one block: the base
  struct avail* head = &pool->avail[pool->kval_m];
  TEST_ASSERT_EQUAL_PTR(head->next, head->prev); // there's exactly one in the list
  TEST_ASSERT_NOT_EQUAL_PTR(head, head->next);   // it shouldn't point to itself
  TEST_ASSERT_EQUAL_UINT16(BLOCK_UNUSED, head->tag);
  TEST_ASSERT_EQUAL_UINT16(pool->kval_m, head->kval);

  // Check the block in the highest list
  struct avail* block = head->next;
  TEST_ASSERT_EQUAL_UINT16(BLOCK_AVAIL, block->tag);
  TEST_ASSERT_EQUAL_UINT16(pool->kval_m, block->kval);
  TEST_ASSERT_EQUAL_PTR(block->next, head);
  TEST_ASSERT_EQUAL_PTR(block->prev, head);
}

/**
 * @brief Check the pool to ensure it is "empty," i.e., no free blocks.
 *        That means every free list in pool->avail[i] should point to itself.
 */
static void check_buddy_pool_empty(struct buddy_pool *pool)
{
  for (size_t i = 0; i <= pool->kval_m; i++) {
    TEST_ASSERT_EQUAL_PTR(&pool->avail[i], pool->avail[i].next);
    TEST_ASSERT_EQUAL_PTR(&pool->avail[i], pool->avail[i].prev);
    TEST_ASSERT_EQUAL_UINT16(BLOCK_UNUSED, pool->avail[i].tag);
    TEST_ASSERT_EQUAL_UINT16(i, pool->avail[i].kval);
  }
}

/* ------------------------------------------------------------------
 *  Instructor’s Basic Tests
 * ------------------------------------------------------------------ */

/**
 * @brief Tests to make sure that the struct buddy_pool is correct and all fields
 *        have been properly set after a call to buddy_init.
 */
void test_buddy_init(void)
{
  fprintf(stderr, "->Testing buddy_init\n");

  // Test multiple sizes from MIN_K up to DEFAULT_K
  for (size_t i = MIN_K; i <= DEFAULT_K; i++) {
    size_t size = (UINT64_C(1) << i);
    struct buddy_pool pool;
    buddy_init(&pool, size);
    // Should have exactly one block of size i
    check_buddy_pool_full(&pool);
    buddy_destroy(&pool);
  }
}

/**
 * @brief Allocating 1 byte => we expect it to split down to SMALLEST_K,
 *        then free it and verify we get a single large block again.
 */
void test_buddy_malloc_one_byte(void)
{
  fprintf(stderr, "->Test allocating and freeing 1 byte\n");
  struct buddy_pool pool;
  buddy_init(&pool, (UINT64_C(1) << MIN_K));  // create a smallish pool

  // 1) Allocate 1 byte
  void *mem = buddy_malloc(&pool, 1);
  TEST_ASSERT_NOT_NULL(mem);

  // 2) Free
  buddy_free(&pool, mem);

  // 3) Check that pool is back to full
  check_buddy_pool_full(&pool);

  buddy_destroy(&pool);
}

/**
 * @brief Tests the allocation of one massive block that should consume
 *        the entire memory pool. Then ensures subsequent calls fail
 *        until we free that block.
 */
void test_buddy_malloc_one_large(void)
{
  fprintf(stderr, "->Testing size that will consume entire memory pool\n");
  struct buddy_pool pool;
  // create a pool of 2^MIN_K:
  size_t bytes = UINT64_C(1) << MIN_K;
  buddy_init(&pool, bytes);

  // If the entire block is 2^MIN_K bytes, the overhead for the header is struct avail.
  // So we request: block_size - overhead
  size_t ask = bytes - sizeof(struct avail);

  void *mem = buddy_malloc(&pool, ask);
  TEST_ASSERT_NOT_NULL(mem);

  // We expect the pool to be empty now
  check_buddy_pool_empty(&pool);

  // Verify that a call on an empty pool fails
  void *fail = buddy_malloc(&pool, 8);
  TEST_ASSERT_NULL(fail);
  TEST_ASSERT_EQUAL_INT(ENOMEM, errno);

  // Free the memory
  buddy_free(&pool, mem);
  // Now the pool should be full again
  check_buddy_pool_full(&pool);

  buddy_destroy(&pool);
}

/* ------------------------------------------------------------------
 *  Additional Tests for Coverage
 * ------------------------------------------------------------------ */

/**
 * @brief Test buddy_malloc with size=0. Should return NULL, do nothing.
 */
void test_buddy_malloc_zero(void)
{
  fprintf(stderr, "->Test buddy_malloc(0)\n");
  struct buddy_pool pool;
  buddy_init(&pool, (UINT64_C(1) << MIN_K));

  void *p = buddy_malloc(&pool, 0);
  TEST_ASSERT_NULL(p); // expecting NULL

  check_buddy_pool_full(&pool); // pool should remain unchanged
  buddy_destroy(&pool);
}

/**
 * @brief Test buddy_malloc requesting more than the pool can handle.
 *        Should return NULL and set errno = ENOMEM.
 */
void test_buddy_malloc_too_large(void)
{
  fprintf(stderr, "->Test buddy_malloc too large\n");
  struct buddy_pool pool;
  buddy_init(&pool, (UINT64_C(1) << MIN_K)); // e.g. 2^20 bytes

  // request 2^(MIN_K+1) => bigger than entire pool
  size_t request = (UINT64_C(1) << (MIN_K+1));
  void *p = buddy_malloc(&pool, request);
  TEST_ASSERT_NULL(p);
  TEST_ASSERT_EQUAL_INT(ENOMEM, errno);

  check_buddy_pool_full(&pool); // still should be full
  buddy_destroy(&pool);
}

/**
 * @brief Test that buddy_free(NULL) is safe (no crash, no effect).
 */
void test_buddy_free_null(void)
{
  fprintf(stderr, "->Test buddy_free(NULL)\n");
  struct buddy_pool pool;
  buddy_init(&pool, (UINT64_C(1) << MIN_K));

  // calling buddy_free(NULL) should do nothing
  buddy_free(&pool, NULL);

  check_buddy_pool_full(&pool); // no effect
  buddy_destroy(&pool);
}

/**
 * @brief Allocate and free multiple blocks, checking merges happen properly.
 */
void test_buddy_multiple_allocations(void)
{
  fprintf(stderr, "->Test buddy_multiple_allocations\n");
  struct buddy_pool pool;
  buddy_init(&pool, (UINT64_C(1) << (MIN_K + 1))); // e.g. 2^(MIN_K+1)

  // Allocate two small blocks
  void *a = buddy_malloc(&pool, 32);
  TEST_ASSERT_NOT_NULL(a);
  void *b = buddy_malloc(&pool, 32);
  TEST_ASSERT_NOT_NULL(b);

  // Freed in order
  buddy_free(&pool, a);
  buddy_free(&pool, b);

  // Freed both => should coalesce back into a single big block
  check_buddy_pool_full(&pool);

  buddy_destroy(&pool);
}

/**
 * @brief Test partial split and free (verifying internal splitting).
 *        e.g., allocate something that forces splits, free it, ensure merges back.
 */
void test_buddy_split_and_merge(void)
{
  fprintf(stderr, "->Test buddy_split_and_merge\n");
  struct buddy_pool pool;
  // e.g. 2^(MIN_K + 2) => 4 times bigger than minimal
  buddy_init(&pool, (UINT64_C(1) << (MIN_K + 2)));

  // Allocate ~128 bytes => we expect lots of splitting down
  void *p = buddy_malloc(&pool, 128);
  TEST_ASSERT_NOT_NULL(p);

  // Now free it. Should coalesce back
  buddy_free(&pool, p);

  // Pool should return to a single free block
  // The entire 2^(MIN_K+2)
  // i.e. check that it's "full."
  // If merges didn't happen, you wouldn't get one free block at the top level.
  check_buddy_pool_full(&pool);

  buddy_destroy(&pool);
}

/* ------------------------------------------------------------------
 *  (Optional) Realloc Tests
 * ------------------------------------------------------------------ */

/**
 * @brief If you implemented buddy_realloc, test it. Otherwise, skip or stub out.
 */
void test_buddy_realloc_basic(void)
{
  fprintf(stderr, "->Test buddy_realloc_basic\n");
  struct buddy_pool pool;
  buddy_init(&pool, (UINT64_C(1) << MIN_K));

  // allocate some block
  void *p = buddy_malloc(&pool, 100);
  TEST_ASSERT_NOT_NULL(p);

  // expand it
  void *p2 = buddy_realloc(&pool, p, 200);
  // if buddy_realloc is not implemented, p2 might be NULL. If you implemented it, do more checks:
  if (p2 != NULL) {
    // Freed old p, allocated new one => check we can write safely
    memset(p2, 0xAA, 200);
    // try to shrink it
    void *p3 = buddy_realloc(&pool, p2, 50);
    TEST_ASSERT_NOT_NULL(p3);
    memset(p3, 0xBB, 50);
    // free it
    buddy_free(&pool, p3);
  }

  // end => check it merges back
  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
}

/* ------------------------------------------------------------------
 *  Test Runner
 * ------------------------------------------------------------------ */
int main(void) {
  time_t t;
  unsigned seed = (unsigned)time(&t);
  fprintf(stderr, "Random seed:%u\n", seed);
  srand(seed);
  printf("Running Buddy Allocator Tests.\n");

  UNITY_BEGIN();

  /* Provided Tests */
  RUN_TEST(test_buddy_init);
  RUN_TEST(test_buddy_malloc_one_byte);
  RUN_TEST(test_buddy_malloc_one_large);

  /* Additional Tests */
  RUN_TEST(test_buddy_malloc_zero);
  RUN_TEST(test_buddy_malloc_too_large);
  RUN_TEST(test_buddy_free_null);
  RUN_TEST(test_buddy_multiple_allocations);
  RUN_TEST(test_buddy_split_and_merge);
  RUN_TEST(test_buddy_realloc_basic);  // only if you implemented buddy_realloc

  return UNITY_END();
}
