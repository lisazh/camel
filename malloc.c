#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <math.h>
#include <sched.h>

#include "memlib.h"
#include "malloc.h"
#include "mm_thread.h"


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

// toggling debug print
#if 0
#define DEBUG(...) do {fprintf(stderr, __VA_ARGS__);} while(0)
#else
#define DEBUG(...) do {} while(0)
#endif


// ---------------------------------------------------------------------
// Wrappers for locking and unlocking
// ---------------------------------------------------------------------

// the mem_sbrk lock
#if 0
typedef pthread_spinlock_t mem_sbrk_lock_t;
#define MEM_SBRK_LOCK_INIT(lock) pthread_spin_init(lock, PTHREAD_PROCESS_SHARED)
#define LOCK_MEM_SBRK(lock) pthread_spin_lock(lock)
#define UNLOCK_MEM_SBRK(lock) pthread_spin_unlock(lock)
#else
typedef pthread_mutex_t mem_sbrk_lock_t;
#define MEM_SBRK_LOCK_INIT(lock) pthread_mutex_init(lock, NULL)
#define LOCK_MEM_SBRK(lock) pthread_mutex_lock(lock)
#define UNLOCK_MEM_SBRK(lock) pthread_mutex_unlock(lock)
#endif


// ---------------------------------------------------------------------
// Shared global variables, some of which are set during mm_init
// ---------------------------------------------------------------------

// Lock for mem_sbrk
mem_sbrk_lock_t mem_sbrk_lock;

#define CACHELINE_SIZE 64
#define SUPERBLOCK_SIZE 4096

// our size classes will be powers of this
#define SIZE_CLASS_BASE 2

// the smallest size we'll start with (in bytes)
#define MIN_SIZE_CLASS 8

// an upper bound on the biggest size class
#define MAX_SIZE_CLASS (DSEG_MAX)

// an upper bound on the number of size classes we'll have
#define MAX_NUM_SIZE_CLASS 128

// array of the sizes of the size classes
size_t *SIZE_CLASSES = NULL;

// number of size classes that we have
int NUM_SIZE_CLASSES = 0;

// how much space is available in a superblock for allocation
size_t SB_AVAILABLE = 0;

// if a heap has less or exactly this number of superblocks
// then it won't give any of them up to the global heap
#define SB_RESERVE 4

// the denominator for fullness buckets e.g. 1/8 full, 2/8 full, etc...
#define FULLNESS_DENOM 3

// size of heap metadata structure padded to cache line
size_t HEAP_SIZE = 0;

// array of heap arrays
struct heap_t;
typedef struct heap_t heap;
heap **HEAPS = NULL;

int NUM_PROCESSORS = 0;

// pointer to where superblocks start and the heap structures end
char *SUPERBLOCK_START = NULL;

// ---------------------------------------------------------------------
// Helper functions for various memory alignments
// ---------------------------------------------------------------------

// round the requested size up to the nearest multiple of the stride

size_t round_to(size_t s, size_t stride) {
	return (s + stride - 1) / stride * stride;
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

// we want to make sure the freelist is at most 8 bytes so we don't use pointers
struct freelist_t {
	/* next isn't a pointer to the next freelist node
	 * instead, it's an offset to the next freelist node, from the start of the superblock
	 * because the header is at the start of the superblock, a value of 0 is invalid
	 * so 0 will be used in a similar manner to NULL for pointers
	 */
	unsigned int next;
	
	/* n is how many contiguous blocks this freelist node spans
	 * it's useful because we can initialize a superblock with a single freelist node
	 * but as blocks are allocated and freed, the freelist will end up with many
	 * discrete sub-blocks, that are not coalesced, but that is not a problem since
	 * we never allocate more than one sub-block at a time.
	 */
	unsigned int n;
};
typedef struct freelist_t freelist;

struct superblock_t {
	// Lock for this superblock
	// this lock is used to protect all fields except next,prev,bucketnum
	pthread_mutex_t lock;
	
	// next in the doubly linked list in the free bucket
	struct superblock_t *next;
	
	// previous in the doubly linked list
	struct superblock_t *prev;
	
	// head of the freelist of this superblock
	freelist *head;
	
	// number of bytes of allocated memory
	size_t allocated;
	
	// which heap owns this
	int owner;
	
	// which size class this superblock is 
	int size_class;
	
	// which bucket this superblock is in 
	int bucketnum;
	
	// how many are in the array, if this is the first of an array
	// this will be 1 for a regular single superblock
	int n;
	
};
typedef struct superblock_t superblock;

// size of the superblock header
#define SUPERBLOCK_HSIZE (sizeof(superblock))

// if a superblock has less than threshold allocated, we move it to global heap
#define ALLOC_THRESHOLD (SUPERBLOCK_SIZE/8)

// initialize a superblock
// given the heap that owns this, what size class this is, and how many in the array
// given a mem_sbrk region of memory that is assumed to fit
int init_superblock(int owner, int size_class, int n, char *sb) {
	assert(owner >= 0 && owner <= NUM_PROCESSORS);
	assert(size_class >= 0 && size_class < NUM_SIZE_CLASSES);
	assert(n > 0);
	assert(sb != NULL);
	
	// initialize the header
	superblock *header = (superblock*)sb;
	header->owner = owner;
	header->bucketnum = -2; // some invalid value that needs to be overridden
	header->size_class = size_class;
	header->n = n;
	header->next = NULL;
	header->prev = NULL;
	header->allocated = 0;
	pthread_mutex_init(&header->lock, NULL);
	
	// initialize the freelist with one big free chunk
	size_t freestart = round_to(SUPERBLOCK_HSIZE, 8);
	size_t class_size = SIZE_CLASSES[size_class];
	// assume we have enough memory for at least one block
	assert((char*)sb + freestart + class_size <= (sb + n * SUPERBLOCK_SIZE));
	// initialize the first block
	freelist *head = (freelist*)(sb + freestart);
	// find out how many blocks can fit
	head->n = ((n-1) * SUPERBLOCK_SIZE + SB_AVAILABLE) / class_size;
	head->next = 0;
	header->head = head;
	return 0;
}

void debug_superblock(char *ptr) {
	printf("-------------------------------------------------------\n");
	printf("header size: %u\n", SUPERBLOCK_HSIZE);
	size_t freestart = round_to(SUPERBLOCK_HSIZE, 8);
	printf("freestart: %u\n", freestart);
	
	superblock *sb = (superblock*)ptr;
	printf("Owner:%d\n", sb->owner);
	printf("Bucketnum:%d\n", sb->bucketnum);
	printf("Size class:%d, %u\n", sb->size_class, SIZE_CLASSES[sb->size_class]);
	printf("Array n:%d\n", sb->n);
	printf("freelist:%p %d\n", sb->head, (int)((char*)sb->head - ptr));
	printf("allocated:%u\n", sb->allocated);
	printf("prev:%p %d\n", sb->prev, (char*)sb->prev-SUPERBLOCK_START);
	printf("next:%p %d\n", sb->next, (char*)sb->next-SUPERBLOCK_START);
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
	// this lock is for everything in here and for prev,next,bucketnum in superblock
	pthread_mutex_t lock;
	
	// array of fullness buckets
	// ordered from most full to least full
	superblock **buckets[FULLNESS_DENOM];
	
	// stats
	int num_superblocks;
};
//typedef struct heap_t heap;

heap *new_heap() {
	// allocate it from the OS
	heap *h = (heap*)mem_sbrk(HEAP_SIZE);
	assert(h != NULL);
	
	pthread_mutex_init(&h->lock, NULL);
	h->num_superblocks = 0;
	
	// initialize fullness buckets
	size_t free_bucket_size = NUM_SIZE_CLASSES*sizeof(superblock*);
	int i;
	for (i = 0; i < (FULLNESS_DENOM); ++i) {
		h->buckets[i] = (superblock**)((char*)h + sizeof(heap) + i*free_bucket_size);
		int j;
		for (j = 0; j < NUM_SIZE_CLASSES; ++j) {
			h->buckets[i][j] = NULL;
		}
	}
	
	return h;
}

void debug_heap(char *ptr) {
	heap *h = (heap*)ptr;
	printf("-------------------------------------------------------\n");
	printf("Heap info:\n");
	printf("Heap size: %u\n", HEAP_SIZE);
	printf("Partially free superblocks: %d\n", h->num_superblocks);
	printf("Bucket start: %u\n", sizeof(heap));
	int i;
	int j;
	for (i = 0; i < (FULLNESS_DENOM); ++i) {
		//printf("bucket number: %d, pointer address: %p\n", i, h->buckets[i]);
		printf("fb:%d: %u\n", i, (size_t)((char*)h->buckets[i]-(char*)h));
		for (j = 0; j < NUM_SIZE_CLASSES; ++j) {
		  printf("fb:%d,fb:%d: %u\n", i, j, (size_t)h->buckets[i][j]);
		}
	}
}

// ---------------------------------------------------------------------
// Helper functions for finding the right size class
// ---------------------------------------------------------------------

// find which size class s falls into
int find_size_class(size_t s) {
	if (s <= MIN_SIZE_CLASS) {
		return 0;
	}
	// otherwise bigger than the min size class
	double log_base = log((double)s / MIN_SIZE_CLASS) / log(SIZE_CLASS_BASE);
	int candidate = (int)ceil(log_base);
	
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
	/*
	// debugging
	long n;
	for (n = 0; n < NUM_SIZE_CLASSES; ++n) {
		printf("%ld: %u\n", n, SIZE_CLASSES[n]);
	}
	*/
	return request_size;
}



// ---------------------------------------------------------------------
// mm_init, mm_malloc, mm_freechar
// ---------------------------------------------------------------------

int mm_init (void) {
	MEM_SBRK_LOCK_INIT(&mem_sbrk_lock);
	
	if (mem_init()) {
		return -1;
	}
	int size_classes_size = init_size_classes();
	if (size_classes_size < 0) {
		return -1;
	}
	
	// make sure to pad the header if necessary to be 8 byte aligned
	SB_AVAILABLE = SUPERBLOCK_SIZE - round_to(SUPERBLOCK_HSIZE, 8);
	
	// calculate how big the the fullness buckets need to be;
	size_t num_free_buckets = (FULLNESS_DENOM) * NUM_SIZE_CLASSES;
	HEAP_SIZE = round_to_cache(sizeof(heap) + num_free_buckets * sizeof(superblock*));
	
	// calculate number of processors
	NUM_PROCESSORS = getNumProcessors();
	
	// make the shared array of heaps
	// heap 0 is the global heap
	size_t num_heaps = NUM_PROCESSORS+1;
	size_t heaps_array_size = round_to_cache(num_heaps*sizeof(heap*));
	HEAPS = mem_sbrk(heaps_array_size);
	assert(HEAPS != NULL);
	
	// initialize all heaps
	int i;
	for (i = 0; i < num_heaps; ++i) {
		HEAPS[i] = new_heap();
		assert(HEAPS[i] != 0);
	}
	
	//void test_heap();
	//test_heap();
	
	//void test_superblock();
	//test_superblock();
	
	int total_overhead = size_classes_size + heaps_array_size + HEAP_SIZE*(NUM_PROCESSORS+1);
	
DEBUG("Page size: %db\n", mem_pagesize());
DEBUG("Overhead: %db\n", total_overhead);
	
	// pad out the rest so the superblocks will start page aligned
	size_t padding = total_overhead % mem_pagesize();
	if (padding > 0) {
		padding = mem_pagesize() - padding;
	}
	mem_sbrk(padding);
	SUPERBLOCK_START = total_overhead + padding + dseg_lo;
	
DEBUG("Superblock start: %db\n", SUPERBLOCK_START - dseg_lo);
	
	return 0;
}

/*
 * Helper function for allocating a free block.
 * It assumes the given superblock is not completely full.
 * It gets a free block from the superblock and updates the freelist
 * and returns a pointer to the block.
 * Assumes lock on superblock freeblk has been acquired.
 */
void* allocate_block(int sclass, superblock *freeblk) {
	
	void *ret;
	freelist *freespace = freeblk->head;
	assert(freespace != NULL);
	if (freespace->n > 1){
		--freespace->n;
		ret = (char *)freespace + (freespace->n*SIZE_CLASSES[sclass]);
	} else { //freespace->n == 1
		assert(freespace->n == 1);
		ret = freespace;
		if (freespace->next != 0) {
			freeblk->head = (freelist *)((char *)freeblk + freespace->next);
			assert(freeblk->head != NULL);
		} else { //freespace->next == 0
			freeblk->head = NULL;
		}
	}
	freeblk->allocated += SIZE_CLASSES[sclass];
	return ret;
	
}

/*
 * Search function for finding available superblock.
 * Searches fullness buckets for free block of sizeclass sclass.
 * Returns pointer to said superblock.
 * Assumes lock on heap aheap has been acquired.
 */
superblock *search_free(int sclass, heap *aheap, int *bucketnum){
	int i;
	for (i = 0; i< FULLNESS_DENOM; i++) {
		superblock *freeblk = aheap->buckets[i][sclass];
		if (freeblk != NULL){
			*bucketnum = i;
			assert(freeblk->bucketnum == i);
			return freeblk;
		}
	}
	
	return NULL;
}

/*
 * Removes superblock blk from the given bucket.
 * Assume heap lock is held.
 */
void remove_sb_from_bucket(heap *myheap, int bucketnum, int sizeclass, superblock *blk) {
	assert(bucketnum >= 0 && bucketnum < FULLNESS_DENOM);
	superblock *oldnext = blk->next;
	superblock *oldprev = blk->prev;
	if (oldprev == NULL) { //blk is the head of the bucket
		assert(myheap->buckets[bucketnum][sizeclass] == blk);
		myheap->buckets[bucketnum][sizeclass] = oldnext;
	} else {
		// otherwise update it to point past blk and to the next one
		oldprev->next = oldnext;
	}
	if (oldnext != NULL) {
		oldnext->prev = oldprev;
	}
	blk->next = NULL;
	blk->prev = NULL;
	blk->bucketnum = -1;
	// update superblock count
	--myheap->num_superblocks;
}

/*
 * Inserts the given superblock into the head of this bucket.
 * Assume heap lock is held.
 */
void insert_sb_into_bucket(heap *myheap, int bucketnum, int sizeclass, superblock *freeblk) {
	superblock *newnext = myheap->buckets[bucketnum][sizeclass];
	myheap->buckets[bucketnum][sizeclass] = freeblk;
	freeblk->next = newnext;
	freeblk->prev = NULL;
	if (newnext != NULL) {
		// update the new next superblock's prev
		newnext->prev = freeblk;
	}
	freeblk->bucketnum = bucketnum;
	// update superblock count
	++myheap->num_superblocks;
}

/*
 * Given that we've just allocated a block from the first superblock
 * in bucket bucketnum from size class sizeclass, we need to potentially
 * move this superblock to another bucket or remove it completely if it
 * is completely full.
 * 
 * Assume the given heap and superblock is locked.
 */
void update_buckets(heap *myheap, int bucketnum, int sizeclass) {
	superblock *freeblk = myheap->buckets[bucketnum][sizeclass];
	assert(freeblk != NULL);
	if (freeblk->head == NULL) {
		// if the block freelist is empty, then it means this superblock is full
		// so just remove it from the buckets
		remove_sb_from_bucket(myheap, bucketnum, sizeclass, freeblk);
	} else {
		// otherwise the block freelist isn't empty
		// now we have to check whether it got fuller and needs to be moved to another fullness bucket
		double alloc_ratio = (double)freeblk->allocated / (SB_AVAILABLE + (freeblk->n - 1)*SUPERBLOCK_SIZE);
		if (alloc_ratio > (double)(FULLNESS_DENOM - bucketnum)/FULLNESS_DENOM) {
			// move it one bucket over
			assert(bucketnum > 0);
			remove_sb_from_bucket(myheap, bucketnum, sizeclass, freeblk);
			insert_sb_into_bucket(myheap, bucketnum-1, sizeclass, freeblk);
		}
	}
}

void *mm_malloc (size_t size) {
	if (size == 0) {
		return NULL;
	}
	int sizeclass = find_size_class(size);
	if (sizeclass < 0) {
		return NULL;
	}
	int mycpu = sched_getcpu();
	assert(mycpu >= 0 && mycpu < NUM_PROCESSORS);
DEBUG("mm_malloc: cpu %d, size %u, size class %d\n", mycpu, size, sizeclass);
	// check this heap for free block
	heap *myheap = HEAPS[mycpu +1];
	int bucketnum;
	// lock this heap
	pthread_mutex_lock(&myheap->lock);
	superblock *freeblk = search_free(sizeclass, myheap, &bucketnum);
	void *ret = NULL;
	if (freeblk != NULL) {
		pthread_mutex_lock(&freeblk->lock);
		ret = allocate_block(sizeclass, freeblk);
		//potentially move the superblock around to another fullness bucket
		update_buckets(myheap, bucketnum, sizeclass);
		pthread_mutex_unlock(&freeblk->lock);
		pthread_mutex_unlock(&myheap->lock);
		assert(ret != NULL);
		return ret;
	}
DEBUG("mm_malloc: Checking global heap\n");
	// unsuccessful in myheap, so check global heap
	heap *global = HEAPS[0];
	pthread_mutex_lock(&global->lock);
	freeblk = search_free(sizeclass, global, &bucketnum);
	if (freeblk != NULL) {
		// now we've found one, so transfer it over
		// remove from global heap's buckets and add to this heap's buckets
		remove_sb_from_bucket(global, bucketnum, sizeclass, freeblk);
		insert_sb_into_bucket(myheap, bucketnum, sizeclass, freeblk);
		// lock this superblock
		pthread_mutex_lock(&freeblk->lock);
		// since we've locked the superblock we don't need the global heap lock
		pthread_mutex_unlock(&global->lock);
		// change owners
		freeblk->owner = mycpu+1;
		// now we continue as if we found a suitable superblock in our own heap
		ret = allocate_block(sizeclass, freeblk);
		//potentially move the superblock around to another fullness bucket
		update_buckets(myheap, bucketnum, sizeclass);
		pthread_mutex_unlock(&freeblk->lock);
		pthread_mutex_unlock(&myheap->lock);
		assert(ret != NULL);
		return ret;
	} else {
		// otherwise we didn't find anything so release the global heap lock and continue
		pthread_mutex_unlock(&global->lock);
	}
DEBUG("mm_malloc: mem_sbrking\n");
	// unsucessful in global heap too, so get new superblock
	int numblks = 1;
	if (SIZE_CLASSES[sizeclass] > SB_AVAILABLE) {
		numblks += (SIZE_CLASSES[sizeclass] - SB_AVAILABLE + SUPERBLOCK_SIZE - 1) / SUPERBLOCK_SIZE;
	}
	LOCK_MEM_SBRK(&mem_sbrk_lock);
	superblock *newblk = mem_sbrk(SUPERBLOCK_SIZE * numblks);
	UNLOCK_MEM_SBRK(&mem_sbrk_lock);
	if (newblk != NULL) {
		// make sure we're not out of memory, otherwise just return NULL
		init_superblock(mycpu+1, sizeclass, numblks, (char *) newblk);
		// don't need to lock superblock since only this heap knows about it
		ret = allocate_block(sizeclass, newblk);
		if (newblk->head != NULL) {
			// only add to buckets if this isn't full
			insert_sb_into_bucket(myheap, FULLNESS_DENOM-1, sizeclass, newblk);
			update_buckets(myheap, FULLNESS_DENOM - 1, sizeclass);
		} else {
			newblk->bucketnum = -1;
		}
		assert(ret != NULL);
	}
	pthread_mutex_unlock(&myheap->lock);
	return ret;
}

/*
 * Function that indicates that there is a new free space in
 * superblock blk by updating its freelist. 
 * Assumes lock to blk has already been obtained.
 */
void update_freelist(superblock *blk, void *ptr) {
	freelist *currfree = blk->head;
	blk->head = (freelist *)ptr;
	//check what old head was and update stats accordingly
	if (currfree == NULL) {
		blk->head->next = 0;
	} else {
		unsigned int curroff = (unsigned int)((char *)currfree - (char*)blk);
		assert(curroff > 0 && curroff < SUPERBLOCK_SIZE);
		blk->head->next = curroff;
	}
	blk->head->n = 1;
	assert(blk->head != NULL);
}


void mm_free (void *ptr) {
DEBUG("mm_free: start\n");
	//find superblock that this pointer is in
	superblock *thisblk = (superblock *)((((char*)ptr - SUPERBLOCK_START)/SUPERBLOCK_SIZE * SUPERBLOCK_SIZE)+ SUPERBLOCK_START);
	//lock superblock
	pthread_mutex_lock(&thisblk->lock);
	//free this (sub)block and update information
	update_freelist(thisblk, ptr);
	thisblk->allocated -= SIZE_CLASSES[thisblk->size_class];
	//find its owner heap
	int owner = thisblk->owner;
	heap *thisheap = HEAPS[owner];
	assert(owner >= 0 && owner <= NUM_PROCESSORS);
	pthread_mutex_unlock(&thisblk->lock);
	
	// just stop here if this block belongs to the global heap to avoid deadlock
	if (owner == 0) {
		return;
	}
	
	// now have to try to get heap lock first to avoid deadlock with mm_malloc
	pthread_mutex_lock(&thisheap->lock);
	pthread_mutex_lock(&thisblk->lock);
	
	// we can continue if it's still owned by same heap
	// otherwise some other thread intervened so we don't need to do anything
	if (owner == thisblk->owner) {
		int bucketnum = thisblk->bucketnum;
		assert(bucketnum >= -1 && bucketnum < FULLNESS_DENOM);
		//determine how much of this superblock has been allocated
		double alloc_ratio = (double)thisblk->allocated/(SB_AVAILABLE + (thisblk->n - 1)*SUPERBLOCK_SIZE);
		//check if this block should be moved to another fullness bucket
		if (alloc_ratio <= (double)(FULLNESS_DENOM - bucketnum - 1)/FULLNESS_DENOM){
			if (bucketnum >= 0 && bucketnum < FULLNESS_DENOM-1) { //but only if it's not full nor completely empty
DEBUG("mm_free: moving buckets\n");
				remove_sb_from_bucket(thisheap, bucketnum, thisblk->size_class, thisblk);
				assert(thisblk->head != NULL);
				insert_sb_into_bucket(thisheap, bucketnum + 1, thisblk->size_class, thisblk);
			} else if (bucketnum == -1 && thisblk->head != NULL) {
				// need to put it into a bucket if it's not completely full anymore
				insert_sb_into_bucket(thisheap, FULLNESS_DENOM-1, thisblk->size_class, thisblk);
			}
		}

		//check if stuff can be moved to global heap
		if (thisheap->num_superblocks > SB_RESERVE && thisblk->allocated < ALLOC_THRESHOLD){
			assert(thisblk->head != NULL); // shouldn't be full
DEBUG("mm_free: moving to global heap\n");
			//change the owner of this block
			thisblk->owner = 0;
			// find out which bucket it's in
			bucketnum = thisblk->bucketnum;
			assert(bucketnum >= -1 && bucketnum < FULLNESS_DENOM);
			// move to global heap
			heap *global = HEAPS[0];
			pthread_mutex_lock(&global->lock);
			if (bucketnum >= 0) {
				// even though this is free, it may not be in a bucket
				remove_sb_from_bucket(thisheap, bucketnum, thisblk->size_class, thisblk);
			}
			// if the block was empty enough to be moved to global heap, then is empty enough
			// to be put in emptiest bucket.
			insert_sb_into_bucket(global, FULLNESS_DENOM-1, thisblk->size_class, thisblk);
			pthread_mutex_unlock(&global->lock);
	
		}
	}
	
	pthread_mutex_unlock(&thisblk->lock);
	pthread_mutex_unlock(&thisheap->lock);
DEBUG("mm_free: exit\n");
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

void test_heap() {
	int i;
	for (i = 0; i < NUM_PROCESSORS+1; ++i) {
		printf("heap %d:\n", i);
		debug_heap((char*)HEAPS[i]);
	}
}
