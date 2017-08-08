/*
 * mig.h
 *
 *  Created on: Jun 29, 2015
 *      Author: xzl
 *
 *  The lockfree queue implementation for memif.
 *
 *  Reference design: liblfds.
 */

#ifndef MIG_H_
#define MIG_H_

#ifndef __KERNEL__
/* from lfd project */
// TRD : any UNIX with GCC on ARM
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <assert.h>
#else
#include <linux/types.h>
#include <linux/bug.h>
#include <linux/sizes.h>
#define assert(x) BUG_ON(!(x))
#endif

#include "misc-xzl.h"

#if 0 /* useful when huge tlb is in place? */
#define MIG_BLOCK_SIZE 			SZ_16K
#define MIG_BLOCK_OFFSET_BITS	14	 		// how many bits?
#define MIG_BLOCK_NUM_BITS		(32 - 14)	// arm has 32bit word
#endif

// We cannot assume huge page support of a proper granularity (e.g. 64KB).
// (With ARM LAPE, the huge page is 2MB which is perhaps too big for ks2).
//
// Thus:
// 1. there seems no easy way to get BLOCK aligned virt addr through mmap().
// 2. the backing phys pages in a block may be non contiguous.
//
// Therefore, we only can ask the BLOCK start to be PAGE alignment,
// instead of BLOCK alignment.
//
// Despite this, having large blocks is useful in reducing the # of descs and
// communication overhead.

#ifndef atomic_arm_t
#define atomic_arm_t unsigned long
#endif

/* xzl: from linux kernel. armv7 */
#define dsb() __asm__ __volatile__ ("dsb" : : : "memory")
#define dmb() __asm__ __volatile__ ("dmb" : : : "memory")


#define MIG_BLOCK_ALIGN 		SZ_4K
#define MIG_BLOCK_ALIGN_OFFSET	12

#define MIG_BLOCK_ROUND_DOWN(x) (((unsigned long)(x)) & (~(MIG_BLOCK_ALIGN-1)))
#define MIG_BLOCK_ROUND_UP(x) ( (((unsigned long)(x)) + MIG_BLOCK_ALIGN-1)  & (~(MIG_BLOCK_ALIGN-1)) )

typedef unsigned long int                lfds611_atom_t;
#define LFDS611_INLINE                   inline
#define LFDS611_ALIGN(alignment)         __attribute__( (aligned(alignment)) )
#define LFDS611_ALIGN_SINGLE_POINTER     4
#define LFDS611_ALIGN_DOUBLE_POINTER     8
#define LFDS611_BARRIER_COMPILER_LOAD    __asm__ __volatile__ ( "" : : : "memory" )
#define LFDS611_BARRIER_COMPILER_STORE   __asm__ __volatile__ ( "" : : : "memory" )
#define LFDS611_BARRIER_COMPILER_FULL    __asm__ __volatile__ ( "" : : : "memory" )
#define LFDS611_BARRIER_PROCESSOR_LOAD   __sync_synchronize()
#define LFDS611_BARRIER_PROCESSOR_STORE  __sync_synchronize()
#define LFDS611_BARRIER_PROCESSOR_FULL   __sync_synchronize()

#define LFDS611_BARRIER_LOAD   LFDS611_BARRIER_COMPILER_LOAD; LFDS611_BARRIER_PROCESSOR_LOAD; LFDS611_BARRIER_COMPILER_LOAD
/* xzl: requires -march=armv7-a. does not help much */
//#define LFDS611_BARRIER_LOAD  dmb()

#define LFDS611_BARRIER_STORE  LFDS611_BARRIER_COMPILER_STORE; LFDS611_BARRIER_PROCESSOR_STORE; LFDS611_BARRIER_COMPILER_STORE
#define LFDS611_BARRIER_FULL   LFDS611_BARRIER_COMPILER_FULL; LFDS611_BARRIER_PROCESSOR_FULL; LFDS611_BARRIER_COMPILER_FULL

/* test command ID. sent from usr to kernel */
#define MIG_INIT_REGION 0
#define	MIG_FILL_QREQ	1
#define	MIG_EMPTY_QREQ	2
#define MIG_MOVE_SINGLE 3
#define MIG_MOVE_SINGLE_NOREMAP 4
#define MIG_MOVE_SINGLE_MIG 5	/* direct replacement of pte */

typedef uint16_t 	index_t;

typedef struct _mig_desc mig_desc;

#define BADINDEX 0xffff

typedef int16_t		color_t;
#define COLOR_NONE	0  /* do not propagate color within the queue */
#define COLOR_USR	1
#define COLOR_KER	2

extern const char *clr_strings[];
#define color_str(c) clr_strings[c]

//typedef int16_t		ptr_flag_t;
/* when bit set, new node is tainted with tail's color */
//#define PTR_FLAG_CLR	0

// compile with -std=c1x or -std=c11
typedef struct desc_ptr_x86 {
	union {
		struct {
			/* Native pointer. user can directly use this w/o extra indirect.
			   kernel has to validate the ptr before use. However, this
			   requires kernel knows user mapping before doing anything. */
//			mig_desc *ptr;
			index_t index; 		// index into desc_array
			uint16_t count; 	// for ABA
			color_t color;		// queue property.
//			ptr_flag_t qflag;	// misc flags. always propagated
		};
		uint64_t raw; // the raw value
	};
} mig_desc_ptr;

/* the descriptor of an outstanding migration */
#define MIG_DESC_FREE		0
#define MIG_DESC_REQUESTED	1
#define MIG_DESC_COMPLETED	2
#define MIG_DESC_ACKED		3

typedef struct _mig_desc {
	// word 0 -- the part to be replicated if we copy a mig_desc
	union {
		struct {
			/* NB: signed bit field is tricky. e.g., 1bit is 0 or -1 */
			//modified by hongyu
			unsigned long virt_base: (64 - MIG_BLOCK_ALIGN_OFFSET);
			//unsigned long virt_base: (32 - MIG_BLOCK_ALIGN_OFFSET);
			unsigned order: 3; // # of pages in 2**order. up to 2**7 pages == 128M.
			unsigned node: 1; // destination node: see defs in mmzone.h
			unsigned huge_page: 1; // 1->2M page, 0->4k page
			unsigned flag: MIG_BLOCK_ALIGN_OFFSET - 5;	// bookkeeping: desc status, etc.
		};
		//modified by hongyu
		uint64_t word0;
		//uint32_t word0;
	};
	union {
		struct {
			//modified by hongyu
			unsigned long long virt_base_dest: (64 - MIG_BLOCK_ALIGN_OFFSET);
			//unsigned long virt_base_dest: (32 - MIG_BLOCK_ALIGN_OFFSET);
		};
		//modifiyed by hongyu
		uint64_t word1;
		//uint32_t word1;
	};
	// word 2 +
	//unsigned long page_size; // page_size is 4k, 2M, or 1G
	signed char refcnt;
	mig_desc_ptr next;
} mig_desc;

//modified by hym
#define base_to_virt(x) ((unsigned long long)((x) << MIG_BLOCK_ALIGN_OFFSET))
//#define base_to_virt(x) ((unsigned long)((x) << MIG_BLOCK_ALIGN_OFFSET))
#define virt_to_base(x) ((unsigned long long)(x) >> MIG_BLOCK_ALIGN_OFFSET)
//#define virt_to_base(x) ((unsigned long)(x) >> MIG_BLOCK_ALIGN_OFFSET)

/* query desc in place does not work if desc is copied during dequeue() */
#define mark_mig_desc_free(x) (x->flag = MIG_DESC_FREE)
#define mark_mig_desc_req(x) (x->flag = MIG_DESC_REQUESTED)
#define mark_mig_desc_comp(x) (x->flag = MIG_DESC_COMPLETED)

/* work queue metadata */
typedef struct _mig_queue {
	mig_desc_ptr tail;
	mig_desc_ptr head;

	atomic_arm_t aba_counter;

	/* more? statistics? */
} mig_queue;

typedef struct _mig_free_list {
	/* only operate one end of the free list */
	mig_desc_ptr top;
	atomic_arm_t aba_counter, element_counter;
} mig_free_list;

static inline uint64_t __dwcas(volatile uint64_t *dest, uint64_t oldval,
		uint64_t newval)
{
	/* XXX todo */
	return 0;
}

/* overall memory layout of the shared pages */
typedef struct {
	uint32_t ndescs;

	/* freelist metadata */
	mig_free_list freelist;

	/* metadata of various queues */
	mig_queue qreq;
	mig_queue qissued;
	mig_queue qcomp;
	mig_queue qerr;
	mig_queue qacked;

	/* the actual descs -- must come at last */
	mig_desc desc_array[0];
} mig_region;

static inline index_t desc_to_index(mig_region *region, mig_desc *desc)
{
	assert(region);
	assert(desc);
	//modified by hongyu
	assert((uint64_t)desc >= (uint64_t)(region->desc_array));
	//assert((uint32_t)desc >= (uint32_t)(region->desc_array));

	return (desc - region->desc_array);
}

static inline mig_desc *index_to_desc(mig_region *region, index_t index)
{
	assert(region);
	assert(index < region->ndescs);

	return region->desc_array + index;
}

/* ---- region ---- */
mig_region *init_migregion(void *pages, int size);

/* --- free list ---- */
mig_free_list *freelist_init(mig_region *r);
mig_free_list *freelist_add(mig_region *r, mig_desc *desc);
mig_desc *freelist_remove(mig_region *r);

/* --- migration queue ---- */
mig_queue *migqueue_create(mig_region *r, mig_queue *queue, mig_desc *dummy);
mig_queue *migqueue_create_color(mig_region *r, mig_queue *queue,
		mig_desc *dummy, color_t c);
color_t mig_enqueue(mig_region *r, mig_queue *queue, mig_desc *desc);
mig_desc *mig_dequeue(mig_region *r, mig_queue *queue);
color_t migqueue_set_color_if_empty(mig_region *r, mig_queue *q, color_t c);

/* Get the queue's color, by reading from the head node.
 * Note that this is just a "snapshot" -- queue may change quickly.
 */
static inline color_t migqueue_color(mig_region *r, mig_queue *q)
{
	mig_desc_ptr head, next;
	assert(r && q);
	head = q->head;
	next = index_to_desc(r, head.index)->next;
	return next.color;
}

/* Return a hint by taking a snapshot of the head node. No guarantee though. */
static inline int migqueue_is_empty(mig_queue *q)
{
	return (q->head.index == q->tail.index);
}

/* ---- refcnt (no longer needed since we do replication in dequeuing)------ */
/* return: the updated refcnt */
static inline signed char desc_get(mig_desc *d)
{
#if 0
	return __sync_add_and_fetch(&(d->refcnt), 1);
#endif
	return 0;
}

/* return: the updated refcnt
 * free @desc if its refcnt drops to 0 */
static inline signed char desc_put(mig_region *r, mig_desc *d)
{
#if 0
	signed char ret;
	ret = __sync_sub_and_fetch(&(d->refcnt), 1);
	assert(ret >= 0);

	if (ret == 0) /* is it a good idea to eagerly free? */
		freelist_add(r, d);

	return ret;
#endif
	return 0;
}

/* ----- high level user API (migusr.c) ------------ */
mig_desc *memif_start_replication(int fd, mig_region *r, void* src,
		void *dst, unsigned order);
int memif_open(int nr, mig_region **region);

/* ----- dma transfers see edma.h ------- */

#endif /* MIG_H_ */
