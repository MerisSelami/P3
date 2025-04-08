#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "lab.h"

#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);          \
    } while (0)

/**
 * @brief Convert requested bytes to the correct K value s.t. 2^k >= bytes + overhead.
 *        We do not allow using the math library, so we use bit-shifts/loops.
 *
 * @param bytes the number of *user* bytes requested (not counting overhead).
 * @return size_t the K value (exponent) so that 2^K is >= bytes + sizeof(struct avail).
 */
size_t btok(size_t bytes)
{
    // If the user requests 0 bytes, we can just return 0 so that the caller can handle it.
    if (bytes == 0)
    {
        return 0;
    }

    // We must include space for the header (the struct avail at the start of each allocated block).
    // So the total we must fit into our block is:
    size_t needed = bytes + sizeof(struct avail);

    // Start k at 0; increment until 2^k >= needed.
    size_t k = 0;
    // Use 64-bit shift for safety and avoid math library usage:
    while ((UINT64_C(1) << k) < needed)
    {
        k++;
    }
    return k;
}

/**
 * @brief Compute the buddy of a given block.
 *        The buddy is found by XOR'ing the block's offset (from base) with 2^kval.
 *
 * @param pool  The memory pool (needed for the base pointer).
 * @param block A pointer to the block whose buddy we want.
 * @return struct avail* The buddy block's pointer.
 */
struct avail *buddy_calc(struct buddy_pool *pool, struct avail *block)
{
    // The offset of 'block' from pool->base.
    uintptr_t offset = (uintptr_t)block - (uintptr_t)pool->base;
    // The size of this block is 2^(block->kval).
    size_t size = (UINT64_C(1) << block->kval);

    // XOR the offset with size to get the buddy's offset.
    uintptr_t buddy_offset = offset ^ size;
    // Convert that offset back to a pointer.
    return (struct avail *)((uintptr_t)pool->base + buddy_offset);
}

/**
 * @brief Remove a block from the circular linked list for its size class.
 *
 * @param block The block to remove.
 */
static void remove_block(struct avail *block)
{
    block->prev->next = block->next;
    block->next->prev = block->prev;
    block->next = block->prev = block; // Not strictly necessary, but can help debugging.
}

/**
 * @brief Insert a block into the free list for block->kval (circular list).
 *
 * @param pool The memory pool.
 * @param block The block to insert.
 */
static void insert_block(struct buddy_pool *pool, struct avail *block)
{
    struct avail *head = &pool->avail[block->kval];
    block->next = head->next;
    block->prev = head;
    head->next->prev = block;
    head->next = block;
}

/**
 * @brief The buddy_malloc function. Finds a suitable free block (>= requested size),
 *        splits as needed, and returns a pointer to user memory.
 *
 * @param pool The buddy pool.
 * @param size The number of bytes requested.
 * @return void* Pointer to the allocated user memory (block + 1), or NULL on failure.
 */
void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    // Basic sanity checks
    if (pool == NULL || size == 0)
    {
        return NULL;
    }

    // Convert requested size to a minimal K
    size_t k = btok(size);
    // Ensure that we don't go below the smallest block size we support:
    if (k < SMALLEST_K)
    {
        k = SMALLEST_K;
    }
    // If k is larger than the pool can handle, we cannot allocate:
    if (k > pool->kval_m)
    {
        errno = ENOMEM;
        return NULL;
    }

    // Find a free block from size class k up to pool->kval_m
    size_t i = k;
    while (i <= pool->kval_m && pool->avail[i].next == &pool->avail[i])
    {
        i++;
    }
    // If i went beyond pool->kval_m, there's no suitable block:
    if (i > pool->kval_m)
    {
        errno = ENOMEM;
        return NULL;
    }

    // We found a free list at size i that has a block. Remove the first block from that list.
    struct avail *block = pool->avail[i].next; // the actual free block
    remove_block(block);

    // While i is still bigger than k, split the block down.
    while (i > k)
    {
        // Split the block into two halves. Each half is now size class (i-1).
        i--;
        size_t split_size = (UINT64_C(1) << i);

        // The buddy for the "left" half is offset ^ split_size
        // But let's just compute it directly:
        struct avail *buddy = (struct avail *)((uintptr_t)block + split_size);

        // Mark the buddy as free, size class = i
        buddy->tag = BLOCK_AVAIL;
        buddy->kval = i;
        // Insert the buddy block into the free list for size i
        insert_block(pool, buddy);

        // Update the original block to the left half
        block->kval = i;
    }

    // Now block->kval = k
    // Mark the block as reserved
    block->tag = BLOCK_RESERVED;

    // Return the memory region after the header
    return (void *)(block + 1);
}

/**
 * @brief Frees a block previously allocated with buddy_malloc.
 *        Merges with its buddy if possible, repeating up to the largest size class.
 *
 * @param pool The buddy pool
 * @param ptr  The user pointer (which is block + 1 internally).
 */
void buddy_free(struct buddy_pool *pool, void *ptr)
{
    // If the user gave a null pointer, do nothing.
    if (ptr == NULL)
    {
        return;
    }

    // The block header is right before 'ptr'
    struct avail *block = (struct avail *)ptr - 1;
    // Mark it as available
    block->tag = BLOCK_AVAIL;

    size_t i = block->kval;
    // Try repeatedly to merge with buddy if buddy is free and is the same size
    while (i < pool->kval_m)
    {
        struct avail *buddy = buddy_calc(pool, block);

        // Buddy is considered mergeable if:
        // 1) It's also marked available
        // 2) It has the same kval
        // 3) It's the same size class, i
        if (buddy->tag == BLOCK_AVAIL && buddy->kval == i)
        {
            // Remove buddy from its free list
            remove_block(buddy);

            // The lower address of block/buddy becomes the combined block
            if (buddy < block)
            {
                block = buddy; // buddy is the lower address
            }
            // Now the combined block is size i+1
            block->kval = i + 1;
            i = block->kval;
        }
        else
        {
            // Can't merge. Stop.
            break;
        }
    }

    // Insert the final (possibly merged) block into free list
    insert_block(pool, block);
}

/**
 * @brief This is a simple version of realloc (Optional for undergraduates).
 *
 * @param pool The memory pool
 * @param ptr  The user memory
 * @param size The new size requested
 * @return void* pointer to the new user memory
 */
void *buddy_realloc(struct buddy_pool *pool, void *ptr, size_t size)
{
    // Optional for undergrads. Provided for completeness.
    // A simple approach:
    // 1) If ptr == NULL, just buddy_malloc a new block.
    // 2) If size == 0, free the old block and return NULL.
    // 3) Otherwise, buddy_malloc a new block of 'size', memcpy the old data,
    //    free the old block, return the new pointer.
    if (pool == NULL)
    {
        return NULL;
    }
    if (ptr == NULL)
    {
        return buddy_malloc(pool, size);
    }
    if (size == 0)
    {
        buddy_free(pool, ptr);
        return NULL;
    }

    // Old block
    struct avail *old_block = (struct avail *)ptr - 1;
    size_t old_size = (UINT64_C(1) << old_block->kval) - sizeof(struct avail);

    // If the old block is already big enough, just return ptr.
    if (old_size >= size)
    {
        return ptr;
    }

    // Otherwise, allocate a new block
    void *new_ptr = buddy_malloc(pool, size);
    if (new_ptr != NULL)
    {
        // Copy the old contents up to the smaller of old_size and new size
        size_t copy_size = (old_size < size) ? old_size : size;
        memcpy(new_ptr, ptr, copy_size);
        buddy_free(pool, ptr);
    }
    return new_ptr;
}

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        // Keep it within our allowed range; note the code above uses (MAX_K - 1) so we do that:
        kval = MAX_K - 1;

    memset(pool, 0, sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);

    // Memory map a block of raw memory
    void *mapped = mmap(
        NULL,
        pool->numbytes,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );
    if (mapped == MAP_FAILED)
    {
        handle_error_and_die("buddy_init: mmap failed");
    }
    pool->base = mapped;

    // Initialize the free-list heads for each size class 0..kval
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = &pool->avail[i];
        pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED; // i.e. "This is a dummy head"
    }

    // Put one free block of size 'kval' into the list
    struct avail *m = (struct avail *)pool->base;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;

    // Link it in the circular list for size=kval
    pool->avail[kval].next = m;
    pool->avail[kval].prev = m;
    m->next = &pool->avail[kval];
    m->prev = &pool->avail[kval];
}

void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (rval == -1)
    {
        handle_error_and_die("buddy_destroy: munmap failed");
    }
    // Zero out so it can’t be reused incorrectly
    memset(pool, 0, sizeof(struct buddy_pool));
}

/**
 * (Optional) Debug print function to visualize the bits. (Unused by default.)
 */
static void printb(unsigned long int b)
{
    size_t bits = sizeof(b) * 8;
    unsigned long int curr = UINT64_C(1) << (bits - 1);
    for (size_t i = 0; i < bits; i++)
    {
        if (b & curr)
            printf("1");
        else
            printf("0");
        curr >>= 1L;
    }
}

/**
 * Entry point for an optional main, not strictly needed if you’re only doing unit tests.
 */
int myMain(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    // You can put some debug usage here, if desired.
    return 0;
}
