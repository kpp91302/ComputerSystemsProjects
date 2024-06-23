/*
  fix this header --- README
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
    int inuse : 1; // inuse bit
    int size : 31; // size of block, in words
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
    struct boundary_tag header;
    union
    {
        struct
        {
            struct block *prev;
            struct block *next;
        };
        char payload[0];
    };
};

/* Basic constants and macros */
#define WSIZE 4             /* Word and header/footer size (bytes) */
#define DSIZE 8             /* double size of wsize */
#define MIN_BLOCK_SIZE 16   /*minimum size of a block*/
#define CHUNKSIZE (1 << 12) /* Extend heap by this amount (words) */

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
static struct block *free_listp = 0; /* Pointer to first free block */
static struct block *epilogue = 0;   /* Pointer to epilogue block */
static int free_blocks_available = 0;

/* Function prototypes for internal helper routines */
static struct block *extend_heap(size_t words);
static void place(struct block *bp, size_t asize);
static struct block *find_fit(size_t asize);
static struct block *coalesce(struct block *bp);
/* our functions*/
static void insert_free_block(struct block *bp);
static void remove_free_block(struct block *bp);

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
/*TODO: fix this*/
static struct boundary_tag *get_footer(struct block *blk)
{
    /*
    retrun (void*)blk + WSIZE * blk->header.size - sizeof(struct boundary_tag
    */
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
    /* Create the initial empty heap*/
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1)
    {
        return -1;
    }
    // alignment padding
    // PUT(heap_listp, 0);
    (*(unsigned int *)heap_listp) = 0;

    // Prologue header
    // PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1));
    (*(unsigned int *)(heap_listp + (1 * WSIZE))) = ((DSIZE) | 1);

    // prologue foot
    // PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1));
    (*(unsigned int *)(heap_listp + (2 * WSIZE))) = ((DSIZE) | 1);

    // epilogue header
    // PUT(heap_listp + (3 * WSIZE), PACK(0, 1));
    (*(unsigned int *)(heap_listp + (3 * WSIZE))) = ((0) | 1);

    // for checkheap
    // epilogue = heap_listp + (3 * WSIZE);
    epilogue = (struct block *)(heap_listp + (3 * WSIZE));
    heap_listp += (2 * WSIZE);

    // initialize global list
    free_listp = 0;
    free_blocks_available = 0;

    // extend heap with a free block of CHUNKSIZE words
    if (extend_heap(CHUNKSIZE) == NULL)
    {
        return -1;
    }
    return 0;
}

/*
 * mm_malloc - Allocate a block with at least size bytes of payload
 */
void *mm_malloc(size_t size)
{
    // local vars
    size_t asize;      // adjusted block size
    size_t extendsize; // amount to extend heap if no fit
    struct block *bp;

    // initialize heap if not initialized
    if (heap_listp == 0)
    {
        mm_init();
    }
    // ignore spurious requests
    if (size == 0)
    {
        return NULL;
    }
    // if if size is less than minimum block, set to min
    if (size <= DSIZE)
    {
        asize = MIN_BLOCK_SIZE;
    }
    else // else align size to 16
    {
        asize = align(size); // align size to 16, may need to update this
    }
    //  if there is a fit, place it and return
    if ((bp = find_fit(asize)) != NULL)
    {
        place(bp, asize);
        return bp->payload;
    }
    // else there is no fit, extend heaper by gigger of either asize or chunksize
    if (asize > CHUNKSIZE)
    {
        extendsize = asize;
    }
    else
    {
        extendsize = CHUNKSIZE;
    }
    /* comment this*/
    if ((bp = extend_heap(extendsize)) == NULL)
    {
        return NULL;
    }
    // place the block and return
    place(bp, asize);
    return bp->payload;
}

/*
 * mm_free - Free a block
 */
void mm_free(void *bp)
{
    // if freeing nothing
    if (bp == 0)
    {
        return;
    }
    // if heap isnt initalized yet
    if (heap_listp == 0)
    {
        return;
    }
    // get size of block
    size_t size = blk_size(bp);

    // set alloc bit and size of block
    // PUT(HEADER(bp), PACK(size, 0));

    // PUT(FOOTER(bp), PACK(size, 0));
    // coalesce the block
    coalesce(bp);
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
        insert_free_block(bp); // added
        return bp;
    }

    else if (prev_alloc && !next_alloc)
    { /* Case 2 */
        // combine this block and next block by extending it
        remove_free_block(next_blk(bp)); // added
        mark_block_free(bp, size + blk_size(next_blk(bp)));
    }

    else if (!prev_alloc && next_alloc)
    { /* Case 3 */
        // combine previous and this block by extending previous
        remove_free_block(prev_blk(bp)); // added
        bp = prev_blk(bp);
        mark_block_free(bp, size + blk_size(bp));
    }

    else
    { /* Case 4 */
        // combine all previous, this, and next block into one
        remove_free_block(next_blk(bp)); // added
        remove_free_block(prev_blk(bp)); // added
        mark_block_free(prev_blk(bp),
                        size + blk_size(next_blk(bp)) + blk_size(prev_blk(bp)));
        bp = prev_blk(bp);
    }
    insert_free_block(bp); // added
    return bp;
}

/*
 * mm_realloc - Naive implementation of realloc
 */
void *mm_realloc(void *ptr, size_t size)
{
    // local vars

    // if ptr is null, realloc is same as malloc

    // if size is 0, realloc is same as free

    // check if we need to increase size of block

    // if shrinking the block will give enough room for a free block

    // if space is too small to be a free block, return nothign needs to be done
}

/*
 * checkheap - We don't check anything right now.
 */
void mm_checkheap(int verbose)
{
}

/*
 * The remaining routines are internal helper routines
 */

/*
 * extend_heap - Extend heap with free block and return its block pointer
 */
static struct block *extend_heap(size_t words)
{
    void *bp = mem_sbrk(words * WSIZE);

    if (bp == NULL)
        return NULL;

    /* Initialize free block header/footer and the epilogue header.
     * Note that we overwrite the previous epilogue here. */
    struct block *blk = (struct block *)(((void *)bp + 8) - sizeof(struct block));
    mark_block_free(blk, words);
    next_blk(blk)->header = FENCE;
    epilogue->header.size = 0;
    epilogue->header.inuse = 1;

    /* Coalesce if the previous block was free */
    insert_free_block(blk);
    return coalesce(blk);
}

/*
 * place - Place block of asize words at start of free block bp
 *         and split if remainder would be at least minimum block size
 */
static void place(struct block *bp, size_t asize)
{
    size_t csize = blk_size(bp);

    if ((csize - asize) >= MIN_BLOCK_SIZE_WORDS)
    {
        remove_free_block(bp); // added
        mark_block_used(bp, asize);
        bp = next_blk(bp);
        mark_block_free(bp, csize - asize);
        insert_free_block(bp); // added
    }
    else
    {
        remove_free_block(bp); // added
        mark_block_used(bp, csize);
    }
}

/*
 * find_fit - Find a fit for a block with asize words
 */
static struct block *find_fit(size_t asize)
{
    /* First fit search */
    for (struct block *bp = free_listp; bp != NULL; bp = bp->next)
    {
        if (blk_size(bp) >= asize)
        {
            return bp;
        }
    }
    return NULL; /* No fit */
}

static void insert_free_block(struct block *bp)
{
    // initialize next and prev for the block
    struct block *next = free_listp;
    struct block *prev = NULL;
    // traverse list and insert block in correct position
    while (next != NULL && bp > next) /*update logic*/
    {
        prev = next;
        next = next->next;
    }
    // case if block should be at beginning
    if (prev == NULL)
    {
        free_listp = bp;
    }
    else
    {
        prev->next = bp;
    }
    // set prev and next pointers
    bp->prev = prev;
    bp->next = next;

    // if block is not inserted at end of list
    // update the prev pointer of the next block
    if (next != NULL)
    {
        next->prev = bp;
    }
}

static void remove_free_block(struct block *bp)
{
    // case if block is at beginning of list
    if (bp->prev == NULL)
    {
        free_listp = bp->next;
    }
    else
    {
        bp->prev->next = bp->next;
    }

    // if block is not at end of list update the prev pointer of the next block
    if (bp->next != NULL)
    {
        bp->next->prev = bp->prev;
    }
}

team_t team = {
    /* Team name */
    "okey dokey",
    /* First member's full name */
    "Jackson Small",
    "jacksons02@vt.edu",
    /* Second member's full name (leave as empty strings if none) */
    "Kyle Peterson",
    "kyle913@vt.edu",
};
