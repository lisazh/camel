#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>

#include "memlib.h"
#include "malloc.h"


name_t myname = {
     /* team name to be displayed on webpage */
     "camel_case",
     /* Full name of first team member */
     "Zhaohan (Daniel) Guo",
     /* Email address of first team member */
     "daniel.guo@mail.utoronto.ca",
     /* Full name of second team member */
     "Lisa Zhou",
     /* Email address of second team member */
     "lis.zhou@mail.utoronto.ca"
};

/*
 * Some notes
 * 
 * We'll maintain that when we mem_sbrk we'll always be cache aligned and
 * we'll only allocate multiples of the cache line size. Assume dseg_lo
 * starts off cache aligned and page aligned.
 * 
 * Remember to check if request is 0
 * 
 *  will always be superblock size aligned so we can compute
 * the start of the superblock from a user pointer.
 * 
 * */

// ---------------------------------------------------------------------
// Shared global variables that are set during init
// ---------------------------------------------------------------------

// pthread_mutex_t is 24 bytes
// size_t is 4 bytes
// void* is also 4 bytes
// when checked on redwolf and lab computers

// Lock for mem_sbrk
pthread_mutex_t mem_sbrk_lock = PTHREAD_MUTEX_INITIALIZER;

#define CACHELINE_SIZE 64
#define SUPERBLOCK_SIZE 8192

// our size classes will be powers of this
// we may try values of 1.2 and 1.5 as well
#define SIZE_CLASS_BASE 2

// the smallest size we'll start with (in bytes)
#define MIN_SIZE_CLASS 8

// an upper bound on the biggest size class
// use DSEG_MAX which hopefully will work
#define MAX_SIZE_CLASS (DSEG_MAX)

// an upper bound on the number of size classes we'll have
#define MAX_NUM_SIZE_CLASS 256

// array of the sizes of the size classes
size_t *SIZE_CLASSES = NULL;

// number of size classes that we have
long NUM_SIZE_CLASSES = 0;

// how much space is available in a superblock for allocation
size_t SB_AVAILABLE = 0;

// the denominator for fullness buckets e.g. 1/8 full, 2/8 full, etc...
#define FULLNESS_DENOM 8

// ---------------------------------------------------------------------
// Helper functions for various memory alignments
// ---------------------------------------------------------------------

// round the requested size up to the nearest multiple of the stride

size_t round_to(size_t s, size_t stride) {
	size_t overflow = s % stride;
	return overflow ? s + stride - overflow: s;
}

size_t round_to_cache(size_t s) {
	return round_to(s, CACHELINE_SIZE);
}

size_t round_to_superblock(size_t s) {
	return round_to(s, SUPERBLOCK_SIZE);
}

// ---------------------------------------------------------------------
// Superblock structure
// ---------------------------------------------------------------------

// we want to make sure the freelist is at most 8 bytes
// so instead of pointers, we use the offset from the start of the superblock
// i think this can be singly linked
// we also keep track of how many blocks this free chunk spans
struct freelist_t {
	unsigned int next;
	unsigned int n; // ** explain n, and why it important (but useless in long run)
};
typedef struct freelist_t freelist;

struct superblock_t {
	// Lock for this superblock
	// remember to initialize using pthread_mutex_init
	pthread_mutex_t lock;
	
	// for the singly list in the free bucket
	struct superblock_t *next;
	
	// head of the freelist of this superblock
	freelist *head;
	
	// number of bytes of allocated memory
	size_t allocated;
	
	// which heap owns this
	long owner;
	
	// which size class this is
	long size_class;
	
	// how many are in the array, if this is the first of an array
	// this will be 1 for a regular single superblock
	long n;
	
};
typedef struct superblock_t superblock;

// size of the superblock header
#define SUPERBLOCK_HSIZE (sizeof(superblock))



// if a superblock has less than threshold allocated, we move it to global heap
#define ALLOC_THRESHOLD (SUPERBLOCK_SIZE/FULLNESS_DENOM)

// initialize a superblock
// given the heap that owns this, what size class this is, and how many in the array
// given a mem_sbrk region of memory that is assumed to fit
int init_superblock(long owner, long size_class, long n, char *sb) {
	assert(owner >= 0);
	assert(size_class >= 0 && size_class < NUM_SIZE_CLASSES);
	assert(n > 0);
	assert(sb != NULL);
	
	// initialize the header
	superblock *header = (superblock*)sb;
	header->owner = owner;
	header->size_class = size_class;
	header->n = n;
	header->next = NULL;
	header->allocated = 0;
	pthread_mutex_init(&header->lock, NULL);
	
	// initialize the freelist with one big free chunk
	size_t freestart = round_to(SUPERBLOCK_HSIZE, 8);
	size_t class_size = SIZE_CLASSES[size_class];
	char *one_past_end = sb + n * SUPERBLOCK_SIZE;
	
	// assume we have enough memory for at least one block
	assert((char*)sb + freestart + class_size <= one_past_end);
	// initialize the first block
	freelist *head = (freelist*)(sb + freestart);
	// find out how many blocks can fit
	head->n = ((n-1) * SUPERBLOCK_SIZE + SB_AVAILABLE) / class_size;
	header->head = head;
	
	return 0;
}

void debug_superblock(char *ptr) {
	printf("header size: %u\n", SUPERBLOCK_HSIZE);
	size_t freestart = round_to(SUPERBLOCK_HSIZE, 8);
	printf("freestart: %u\n", freestart);
	
	superblock *sb = (superblock*)ptr;
	printf("Owner:%ld\n", sb->owner);
	printf("Size class:%ld, %u\n", sb->size_class, SIZE_CLASSES[sb->size_class]);
	printf("Array n:%ld\n", sb->n);
	printf("freelist:%ld\n", (long)((char*)sb->head - ptr));
	printf("allocated:%u\n", sb->allocated);
	// print the freelist
	freelist *head = sb->head;
	while (head != NULL && head != (freelist*)ptr) {
		printf("curr %5u, n: %u, next :%5u\n", (unsigned)((char*)head - ptr), head->n, head->next);
		head = (freelist*)(ptr + head->next);
	}
}

// ---------------------------------------------------------------------
// Heap structure
// ---------------------------------------------------------------------

struct heap_t {
	// remember to initialize using pthread_mutex_init
	pthread_mutex_t lock;
	
	// array of fullness buckets
	// ordered from most full to least full
	// the extra bucket is for completely empty superblocks
	superblock **buckets[1 + FULLNESS_DENOM];
};
typedef struct heap_t heap;

int init_heap(char *h) {
	return 0;
}

// ---------------------------------------------------------------------
// Helper functions for finding the right size class
// ---------------------------------------------------------------------

// find which size class s falls into
// we may have to replace this with binary search if we can't use log
long find_size_class(size_t s) {
	double log_base = log((double)s / MIN_SIZE_CLASS) / log(SIZE_CLASS_BASE);
	long candidate = (size_t)ceil(log_base);
	
	if (candidate >= NUM_SIZE_CLASSES) {
		// too big of a request
		return -1;
	}
	// check if previous class can accomdate first
	if (candidate > 0 && SIZE_CLASSES[candidate-1] >= s) {
		return candidate - 1;
	} else {
		// otherwise we know candidate can fit
		return candidate;
	}
}

// ---------------------------------------------------------------------
// Helper functions for mm_init
// ---------------------------------------------------------------------

// initialize all the size classes
int init_size_classes() {
	// allocate enough to hold MAX_NUM_SIZE_CLASS many sizes
	size_t request_size = round_to_cache(sizeof(size_t) * MAX_NUM_SIZE_CLASS);
	SIZE_CLASSES = mem_sbrk(request_size);
	if (SIZE_CLASSES == NULL) {
		return -1;
	}
	
	// calculate the size of each size class
	double size = MIN_SIZE_CLASS;
	NUM_SIZE_CLASSES = 0;
	while ((size_t)ceil(size) <= MAX_SIZE_CLASS && NUM_SIZE_CLASSES < MAX_NUM_SIZE_CLASS) {
		SIZE_CLASSES[NUM_SIZE_CLASSES] = (size_t)ceil(size);
		
		++NUM_SIZE_CLASSES;
		size *= SIZE_CLASS_BASE;
	}
	
	// debugging
	long n;
	for (n = 0; n < NUM_SIZE_CLASSES; ++n) {
		printf("%ld: %u\n", n, SIZE_CLASSES[n]);
	}
	/**/
	return 0;
}



// ---------------------------------------------------------------------
// mm_init, mm_malloc, mm_free
// ---------------------------------------------------------------------

int mm_init (void) {
	if (mem_init()) {
		return -1;
	}
	if (init_size_classes()) {
		return -1;
	}
	
	// make sure to pad the header if necessary to be 8 byte aligned
	SB_AVAILABLE = SUPERBLOCK_SIZE - round_to(SUPERBLOCK_HSIZE, 8);
	
	void test_superblock();
	test_superblock();
	
	return 0;
}

void *mm_malloc (size_t size) {
	assert(0);
}

void mm_free (void *ptr) {
	assert(0);
}

// ---------------------------------------------------------------------
// testing code
// ---------------------------------------------------------------------

// assume mm_init has been called
void test_superblock() {
	char *sb = mem_sbrk(SUPERBLOCK_SIZE);
	init_superblock(0, 0, 1, sb);
	debug_superblock(sb);
}
