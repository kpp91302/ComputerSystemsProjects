/*
Header for P3 - malloc
Jackson Small(jackson02@vt.edu) and Kyle Peterson(kyle913@vt.edu)
Our implementation is a segregated free list with a max block size of 4096 and a minimum block size of 8.
The free blocks are organized into seperate explicit free lists based on their size, this allows for more efficient searching than a typical
explicit free list design.

The block structure is as follows:
    - Each block consists of a header, payload, and footer
    - The header and footer are boundary tags that contain the size of the block and whether the block is in use
    - free blocks are organized into segregated  free lists based on their size

The free list structure is as follows:
    - There are multiple free lists, each with a different size range
    - free blocks are added to the free list based on their size
    - free blocks are removed from the free list when they are allocated
    - free blocks are added to the free list when they are freed
    - the free lists are organized in decreasing order of size, with largest block sizes at the front of the list

Allocation:
    - When a block is allocated, the free list is searched for a block that is large enough to hold the requested size
    - If a block is found, it is removed from the free list and allocated
    - If a block is not found, the heap is extended and the new block is allocated

Freeing:
    - When a block is freed, it is marked as free and added to the free list
    - If the previous or next block is free, the blocks are coalesced into a single block
    - The new block is then added to the appropriate free list

Reallocation:
    - The realloc() function attempts to resize an existing allocated block.
    - It first checks if the block can be extended by coalescing with adjacent free blocks.
    - If extension is not possible, a new block is allocated, and the data is copied from the old block to the new one.
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#include "mm.h"
#include "memlib.h"
#include "config.h"

#include "list.h"

struct boundary_tag
{
    size_t inuse : 1; // inuse bit
    size_t size : 63; // size of block, in words
                      // block size
};

/* FENCE is used for heap prologue/epilogue. */
const struct boundary_tag FENCE = {
    .inuse = -1,
    .size = 0};

/* A C struct describing the beginning of each block.
 * For implicit lists, used and free blocks have the same
 * structure, so one struct will suffice for this example.
 *
 * If each block is aligned at 12 mod 16, each payload will
 * be aligned at 0 mod 16.
 */
struct block
{
    struct boundary_tag header; /* offset 0, at address 12 mod 16 */
    char payload[0];            /* offset 4, at address 0 mod 16 */
    struct list_elem elem;      /* this is the list elem we use to add the block to the free list*/
};

struct free_block_list
{
    struct list free_block_list; // list of free blocks
    struct list_elem elem;       // list elem to add to free list
    size_t list_size;            // size of the list
};

/* Basic constants and macros */
#define WSIZE sizeof(struct boundary_tag) /* Word and header/footer size (bytes) */
#define MIN_BLOCK_SIZE_WORDS 8            /* Minimum block size in words */
#define CHUNKSIZE (1 << 10)               /* Extend heap by this amount (words) */
#define MAX_BLOCK 4096                    /* Maximum block size*/

static inline size_t max(size_t x, size_t y)
{
    return x > y ? x : y;
}

static size_t align(size_t size)
{
    return (size + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
}

static bool is_aligned(size_t size) __attribute__((__unused__));
static bool is_aligned(size_t size)
{
    return size % ALIGNMENT == 0;
}

/* Global variables */
static struct block *heap_listp = 0; /* Pointer to first block */
int count = 0;
static struct list free_blocks;

/* Function prototypes for internal helper routines */
static struct block *extend_heap(size_t words);
static void place(struct block *bp, size_t asize);
static struct block *find_fit(size_t asize);
static struct block *coalesce(struct block *bp);
/* our internal helper functions*/
static void add_free_block(struct block *bp);

/* Given a block, obtain previous's block footer.
   Works for left-most block also. */
static struct boundary_tag *prev_blk_footer(struct block *blk)
{
    return &blk->header - 1;
}

/* Return if block is free */
static bool blk_free(struct block *blk)
{
    return !blk->header.inuse;
}

/* Return size of block is free */
static size_t blk_size(struct block *blk)
{
    return blk->header.size;
}

/* Given a block, obtain pointer to previous block.
   Not meaningful for left-most block. */
static struct block *prev_blk(struct block *blk)
{
    struct boundary_tag *prevfooter = prev_blk_footer(blk);
    assert(prevfooter->size != 0);
    return (struct block *)((void *)blk - WSIZE * prevfooter->size);
}

/* Given a block, obtain pointer to next block.
   Not meaningful for right-most block. */
static struct block *next_blk(struct block *blk)
{
    assert(blk_size(blk) != 0);
    return (struct block *)((void *)blk + WSIZE * blk->header.size);
}

/* Given a block, obtain its footer boundary tag */
static struct boundary_tag *get_footer(struct block *blk)
{
    return ((void *)blk + WSIZE * blk->header.size) - sizeof(struct boundary_tag);
}

/* Set a block's size and inuse bit in header and footer */
static void set_header_and_footer(struct block *blk, int size, int inuse)
{
    blk->header.inuse = inuse;
    blk->header.size = size;
    *get_footer(blk) = blk->header; /* Copy header to footer */
}

/* Mark a block as used and set its size. */
static void mark_block_used(struct block *blk, int size)
{
    set_header_and_footer(blk, size, 1);
}

/* Mark a block as free and set its size. */
static void mark_block_free(struct block *blk, int size)
{
    set_header_and_footer(blk, size, 0);
}

/*
 * mm_init - Initialize the memory manager
 */
int mm_init(void)
{
    assert(offsetof(struct block, payload) == 4);
    assert(sizeof(struct boundary_tag) == 4);

    // init all free lists
    list_init(&free_blocks);
    // start with biggest works to smallest
    for (int i = MAX_BLOCK; i >= MIN_BLOCK_SIZE_WORDS; i /= 2)
    {
        struct free_block_list *b = mem_sbrk(sizeof(struct free_block_list));
        list_init(&b->free_block_list);
        b->list_size = i;
        list_push_back(&free_blocks, &b->elem);
    }

    /* Create the initial empty heap */
    struct boundary_tag *initial = mem_sbrk(2 * sizeof(struct boundary_tag));
    if (initial == NULL)
        return -1;

    /* We use a slightly different strategy than suggested in the book.
     * Rather than placing a min-sized prologue block at the beginning
     * of the heap, we simply place two fences.
     * The consequence is that coalesce() must call prev_blk_footer()
     * and not prev_blk() because prev_blk() cannot be called on the
     * left-most block.
     */
    initial[0] = FENCE; /* Prologue footer */
    heap_listp = (struct block *)&initial[1];
    initial[0] = FENCE; /* Epilogue header */

    /* Extend the empty heap with a free block of CHUNKSIZE bytes */
    if (extend_heap(CHUNKSIZE) == NULL)
        return -1;
    return 0;
}

/*
 * mm_malloc - Allocate a block with at least size bytes of payload
 */
void *mm_malloc(size_t size)
{
    // declare block pointer
    struct block *bp;

    /* Ignore spurious requests */
    if (size == 0)
        return NULL;

    // if heap hasn't been initialized
    if (heap_listp == 0)
    {
        mm_init();
    }

    /* Adjust block size to include overhead and alignment reqs. */
    size_t bsize = align(size + 2 * sizeof(struct boundary_tag)); /* account for tags */
    if (bsize < size)
        return NULL; /* integer overflow */

    /* Adjusted block size in words */
    size_t awords = max(MIN_BLOCK_SIZE_WORDS, bsize / WSIZE); /* respect minimum size */

    /* case 1 - there is a block in the free list we can use*/
    /* Search the free list for a fit */
    if ((bp = find_fit(awords)) != NULL)
    {
        place(bp, awords);
        return bp->payload;
    }

    /* case 2 - there is no block in the free list we can use so we extend the heap*/
    /* No fit found. Get more memory and place the block */
    size_t extendwords = max(awords, CHUNKSIZE); /* Amount to extend heap if no fit */
    if ((bp = extend_heap(extendwords)) == NULL)
        return NULL;

    place(bp, awords);
    return bp->payload;
}

/*
 * mm_free - Free a block
 */
void mm_free(void *bp)
{
    /*assert that mm_init was called*/
    assert(heap_listp != 0);
    if (bp == 0)
        return;

    /* Find block from user pointer */
    struct block *blk = bp - offsetof(struct block, payload);

    mark_block_free(blk, blk_size(blk)); // set the current block to free
    coalesce(blk);                       // coalesce the block if possible
}

/*
 * coalesce - Boundary tag coalescing. Return ptr to coalesced block
 */
static struct block *coalesce(struct block *bp)
{
    bool prev_alloc = prev_blk_footer(bp)->inuse; /* is previous block allocated? */
    bool next_alloc = !blk_free(next_blk(bp));    /* is next block allocated? */
    size_t size = blk_size(bp);

    if (prev_alloc && next_alloc)
    { /* Case 1 */
        // both are allocated, nothing to coalesce
        add_free_block(bp); // add to free list
        return bp;
    }

    else if (prev_alloc && !next_alloc)
    { /* Case 2 */
        //  combine this block and next block by extending it
        struct block *next = next_blk(bp);
        // remove next block from free list
        list_remove(&next->elem);
        // combine and then the new block is added to free list
        mark_block_free(bp, size + blk_size(next)); // or next_blk(bp)
        add_free_block(bp);
        return bp;
    }

    else if (!prev_alloc && next_alloc)
    { /* Case 3 */
        // combine previous and this block by extending previous
        struct block *prev = prev_blk(bp);
        // remove previous block from free list
        list_remove(&prev->elem);
        // combine and then the new block is added to free list
        mark_block_free(prev, size + blk_size(prev));
        add_free_block(prev);
        return prev;
    }

    else
    { /* Case 4 */
        //  combine all three blocks
        struct block *prev = prev_blk(bp);
        struct block *next = next_blk(bp);
        // remove both blocks from free list
        list_remove(&prev->elem);
        list_remove(&next->elem);
        // combine and then the new block is added to free list
        mark_block_free(prev, size + blk_size(prev) + blk_size(next));
        add_free_block(prev);
        return prev;
    }
    return bp;
}

/*
 * mm_realloc - Naive implementation of realloc
 */
void *mm_realloc(void *ptr, size_t size)
{
    /* If size == 0 then this is just free, and we return NULL. */
    if (size == 0)
    {
        mm_free(ptr);
        return 0;
    }

    /* If oldptr is NULL, then this is just malloc. */
    if (ptr == NULL)
    {
        return mm_malloc(size);
    }

    void *newptr;
    size_t bytesize;

    // Find pointer to old block and then essentially coalesce
    struct block *oldblk = ptr - offsetof(struct block, payload);
    bool prev_alloc = prev_blk_footer(oldblk)->inuse;
    bool next_alloc = !blk_free(next_blk(oldblk));

    // right block
    if (!next_alloc)
    {
        struct block *right;
        right = next_blk(oldblk);
        list_remove(&right->elem);
        mark_block_used(oldblk, blk_size(oldblk) + blk_size(next_blk(oldblk)));
    }

    if (blk_size(oldblk) >= size)
        return oldblk->payload;

    // left block
    if (!prev_alloc)
    {
        struct block *left;
        left = prev_blk(oldblk);
        list_remove(&left->elem);
        mark_block_used(left, blk_size(oldblk) + blk_size(left));
        oldblk = left;
    }

    // new block is greater than og, we can return but have to copy mem
    if (blk_size(oldblk) >= size)
    {
        bytesize = blk_size(oldblk) * WSIZE;
        if (size < bytesize)
            bytesize = size;
        memcpy(oldblk->payload, ptr, bytesize);
        return oldblk->payload;
    }

    // not able to coalesce
    newptr = mm_malloc(size);

    // malloc didn't work either so leave original block
    if (!newptr)
        return 0;

    // copy over the data
    struct block *copyblk = ptr - offsetof(struct block, payload);
    bytesize = blk_size(copyblk) * WSIZE; // find number of BYTES, not words
    if (size < bytesize)
        bytesize = size;
    memcpy(newptr, ptr, bytesize);

    mm_free(oldblk->payload);

    return newptr;
}

/*
 * checkheap - We don't check anything right now.
 */
void mm_checkheap(int verbose)
{
    // DECIDED TO NOT IMPLEMENT THIS
}

/*
 * The remaining routines are internal helper routines
 */

/*
 * adds an free block into list
 */
static void add_free_block(struct block *bp)
{
    // make sure block is not null
    assert(bp != 0);
    struct list_elem *e;
    // if free blocks list is empty we add it to the list
    if (list_empty(&free_blocks))
    {
        list_push_back(&free_blocks, &bp->elem);
        return;
    }
    // iterate through free blocks list and place it appropriately
    for (e = list_begin(&free_blocks); e != list_end(&free_blocks); e = list_next(e))
    {
        struct free_block_list *b = list_entry(e, struct free_block_list, elem);
        // if this block size >= min block size
        if (b->list_size <= blk_size(bp))
        {
            list_push_back(&b->free_block_list, &bp->elem);
            return;
        }
    }
}

/*
 * extend_heap - Extend heap with free block and return its block pointer
 */
static struct block *extend_heap(size_t words)
{
    /* Allocate an even number of words to maintain alignment */
    void *bp = mem_sbrk(words * WSIZE);

    /* don't allocate more space if it is not needed - bp is null*/
    if (bp == NULL)
        return NULL;

    /* Initialize free block header/footer and the epilogue header.
     * Note that we overwrite the previous epilogue here. */
    struct block *blk = bp - sizeof(FENCE);
    mark_block_free(blk, words); // make sure the new space is all marked free

    next_blk(blk)->header = FENCE;

    /* Coalesce if the previous block was free */
    return coalesce(blk);
}

/*
 * place - Place block of asize words at start of free block bp
 *         and split if remainder would be at least minimum block size
 */
static void place(struct block *bp, size_t asize)
{
    size_t csize = blk_size(bp);
    // case 1 - we can split the block
    if ((csize - asize) >= MIN_BLOCK_SIZE_WORDS)
    {
        // split the block and set the new block to used
        mark_block_used(bp, asize);
        list_remove(&bp->elem);
        // split the block and set the new block to free and add to free list
        struct block *t = next_blk(bp);
        mark_block_free(t, csize - asize);
        add_free_block(t);
    }
    else // case 2 - we cannot split the block
    {
        // set the block to used and remove from free list
        mark_block_used(bp, csize);
        list_remove(&bp->elem);
    }
}

/*
 * find_fit - Find a fit for a block with asize words
 */
static struct block *find_fit(size_t asize)
{
    struct list_elem *e;

    for (e = list_begin(&free_blocks); e != list_end(&free_blocks); e = list_next(e))
    {
        struct free_block_list *b = list_entry(e, struct free_block_list, elem);
        if (!list_empty(&b->free_block_list) && (b->list_size >= asize || list_prev(e) == list_end(&free_blocks)))
        {
            struct list_elem *elem1;
            struct list_elem *elem2 = list_begin(&b->free_block_list);

            // look through free list to find a block that fits
            // two blocks start at same time and meet in middle
            // is efficient
            for (elem1 = list_rbegin(&b->free_block_list); elem1 != list_rend(&b->free_block_list);
                 elem1 = list_prev(elem1))
            {
                struct block *block1 = list_entry(elem1, struct block, elem);
                struct block *block2 = list_entry(elem2, struct block, elem);

                // good block
                if (blk_size(block1) >= asize)
                {
                    return block1;
                }
                else if (blk_size(block2) >= asize)
                {
                    return block2;
                }

                // stops if list_entries meet in middle or next to each other
                elem2 = list_next(elem2);
                if (elem1 == elem2 || list_next(elem1) == elem2)
                {
                    return NULL;
                }
            }
        }
    }

    return NULL; /* No fit */
}

team_t team = {
    /* Team name */
    "okey dokey i'm gonna chokey",
    /* First member's full name */
    "Kyle Peterson",
    "kyle913@vt.edu",
    /* Second member's full name (leave as empty strings if none) */
    "Jackson Small",
    "jacksons02@vt.edu",
};