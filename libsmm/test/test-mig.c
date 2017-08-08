/*
 * test-mig.c
 *
 * Contains major userspace test functions.
 *
 *  Created on: Jun 30, 2015
 *      Author: xzl
 */

/* toggle debug options */
//#define K2_NO_DEBUG 	1

#define CONFIG_KAGE_GLOBAL_DEBUG_LEVEL 10

#define _GNU_SOURCE		// for MAP_ANONYMOUS

#include <sys/wait.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <stdio.h>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <numa.h>
#include <numaif.h>
#include <poll.h>

#include "log-xzl.h"
#include "mig.h"
#include "misc-xzl.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE SZ_4K
#endif

#define PAGE_COUNT 1024
#define NCPUS 4

#define NR_DEVICES NCPUS

static mig_region * usr_region; /* for user space test only */
static mig_region * regions[NR_DEVICES];
static int configfds[NR_DEVICES];

#define get_freelist(r) (&(r->freelist))
#define get_qreq(r) 	(&(r->qreq))
#define get_desc_array(r)	(r->desc_array)

//static mig_free_list list;
static atomic_arm_t addcnt = 0, rmcnt = 0;

static pthread_t workers[4];	/* Need the space even if we have one CPU! */

static double mysecond()
{
	struct timeval tp;
	struct timezone tzp;
	int i;

	i = gettimeofday(&tp,&tzp);
	assert(i == 0);
	return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

/* allocate the pages that back the migregion.
 * @count: the intended # of descs */
static void *alloc_memory(uint32_t pagecount)
{
	uint32_t pagesize;
	void *p;

	pagesize = sysconf(_SC_PAGE_SIZE);

	p = mmap(0, pagecount * pagesize,
	         (PROT_READ|PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (!p) {
		perror("mmap");
		return NULL;
	}

	V("mmap'd %d pages okay", pagecount);

	return p;
}

/* -------------------------------------------------------------- */

static void *freelist_stress_func(void *p )
{
	mig_desc *desc;
	//  double start, end;
	unsigned int i;

	//  start = mysecond();

	for (i = 0; i < 100000; i++) {
		desc = freelist_remove(usr_region);
		if (!desc)
			break;
		__sync_add_and_fetch(&(rmcnt), 1);

		desc = freelist_remove(usr_region);
		if (!desc)
			break;
		__sync_add_and_fetch(&(rmcnt), 1);

		freelist_add(usr_region, desc);
		__sync_add_and_fetch(&(addcnt), 1);
	}

	//  end = mysecond();
	//  V("done. %.2f sec", end - start);
	return NULL;
}

/* stress the freelist */
static void freelist_stress(void)
{
	int i, ret;
	double start, end;

	V("=============== %s =================", __func__);

	freelist_init(usr_region);
	rmcnt = addcnt = 0;

	start = mysecond();

	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_create(workers + i, NULL, freelist_stress_func, NULL);
		assert(ret == 0);
	}

	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_join(workers[i], NULL);
		assert(ret == 0);
	}

	end = mysecond();

	I("%s done. %d threads. %lu op. %.2f ops (addcnt %lu rmcnt %lu)", __func__,
	  NCPUS, addcnt+rmcnt,
	  1.0 * (addcnt+rmcnt) / (end - start), addcnt, rmcnt);
	if (rmcnt - addcnt == usr_region->ndescs)
		I("this seems right.");
	else
		I("bug?");
}

/* -------------------------------------------------------------- */

/* enqueue: idea from lfd test_queue.c queue_test_enqueuing()

   Multi threads enqueue; single thread deques

   Run one thread per CPU.
   Each thread runs a busy loop, saving thread id and a thread-local counter to
   an element, enqueuing elements (until there are no more elements).

   When we're done, we check that all the elements are present and their numbers
   are increased on a per-thread basis.
*/

static void *enqueue_func(void *p )
{
	mig_desc *desc;
	//  double start, end;
	unsigned int i, localcnt = 0, tid = (int)p;

	for (i = 0; i < 2000; i++) {
		/* grab a free desc */
		desc = freelist_remove(usr_region);
		if (!desc) {
//			V("no more free desc. enque done. %d", i);
			break;
		}

		desc_get(desc);

		/* store info into desc */
		assert(localcnt < (1 << (32 - MIG_BLOCK_ALIGN_OFFSET)) - 1); /* overflow */
		desc->virt_base = localcnt++;
		desc->flag = tid;

		/* enqueue the desc */
		mig_enqueue(usr_region, get_qreq(usr_region), desc);
	}

	return NULL;
}

static void enqueue_test(mig_desc *base)
{
	int i, ret, tid, cnt, sum;
	double start, end;
	int thread_cnts[NCPUS] = {0}; /* used in verification: emulate thread counters */
	mig_desc *desc;

	V("=============== %s =================", __func__);

	freelist_init(usr_region);
	migqueue_create(usr_region, get_qreq(usr_region),
			freelist_remove(usr_region));

	start = mysecond();
	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_create(workers + i, NULL, enqueue_func, (void *)i);
		assert(ret == 0);
	}

	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_join(workers[i], NULL);
		assert(ret == 0);
	}
	end = mysecond();
	V("%s done (%.2f secs).", __func__, end - start);

	/* --- verify --- */
	while ((desc = mig_dequeue(usr_region, get_qreq(usr_region)))) {
		tid = desc->flag;
		cnt = desc->virt_base;

//		V("tid %d actual cnt %d", tid, cnt);

		assert(tid < NCPUS);
//		V("tid %d actual cnt %d expected cnt %d", tid, cnt, thread_cnts[tid]);
		assert(cnt == thread_cnts[tid]);
		thread_cnts[tid] ++;

		desc_put(usr_region, desc);
	}

	I("pass. all threads' counters look fine: ");
	sum = 0;
	for (i = 0; i < NCPUS; i++ ) {
		printf("%d ", thread_cnts[i]);
		sum += thread_cnts[i];
	}
	printf("sum: %d\n", sum);
}

/* Multi threads enqueue. Leave the single-threaded dequeuing work to
 * the kernel. */
static void enqueue_test2(void)
{
	int i, ret;
	double start, end;

	V("=============== %s =================", __func__);

#if 0	// will let kernel do so
	freelist_init(usr_region);
	migqueue_create(usr_region, get_qreq(usr_region),
			freelist_remove(usr_region));
#endif

	start = mysecond();
	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_create(workers + i, NULL, enqueue_func, (void *)i);
		assert(ret == 0);
	}

	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_join(workers[i], NULL);
		assert(ret == 0);
	}
	end = mysecond();
	I("%s done (%.2f secs).", __func__, end - start);

#if 0
	/* --- verify --- */
	while ((desc = mig_dequeue(usr_region, get_qreq(usr_region)))) {
		tid = desc->flag;
		cnt = desc->virt_base;

//		V("tid %d actual cnt %d", tid, cnt);

		assert(tid < NCPUS);
//		V("tid %d actual cnt %d expected cnt %d", tid, cnt, thread_cnts[tid]);
		assert(cnt == thread_cnts[tid]);
		thread_cnts[tid] ++;
	}

	V("pass. all threads' counters look fine: ");
	sum = 0;
	for (i = 0; i < NCPUS; i++ ) {
		printf("%d ", thread_cnts[i]);
		sum += thread_cnts[i];
	}
	printf("sum: %d\n", sum);
#endif
}

/* -------------------------------------------------------------- */

/* idea from lfd test_queue.c queue_test_dequeuing()

  "use a single thread to enqueue every element
   each elements user data is an incrementing counter
   then run one thread per CPU where each busy-works dequeuing
   when an element is dequeued, we check (on a per-thread basis) the
   value deqeued is greater than the element previously dequeued"
*/

static void *dequeue_func(void *p )
{
	mig_desc *desc;
	//  double start, end;
	int i, localcnt = 0;

	for (i = 0; i < 100000; i++) {
//		V("%d: dequeing...(%d)", tid, i);
		desc = mig_dequeue(usr_region, get_qreq(usr_region));
		if (!desc) {
			D("%d: no more desc. dequeue done. %d", tid, i);
			break;
		}

		// check and save to the local counter
		if (desc->virt_base <= localcnt) {
			V("bug: cnt %d localcnt %d", desc->virt_base, localcnt);
			assert(0);
		}

		localcnt = desc->virt_base;

		desc_put(usr_region, desc);
	}

	return (void *)i;
}

static void dequeue_test(mig_desc *base)
{
	int i, ret, cnt, sum;
	double start, end;
	int thread_cnts[NCPUS] = {0};
	mig_desc *desc;

	V("=============== %s =================", __func__);

	freelist_init(usr_region);
	migqueue_create(usr_region, get_qreq(usr_region),
			freelist_remove(usr_region));

	/* single thread enqueue */
	cnt = 1;
	while ((desc = freelist_remove(usr_region))) {
		assert(cnt < (1 << (32 - MIG_BLOCK_ALIGN_OFFSET)) - 1); /* overflow */
		desc->virt_base = cnt++; /* save the increasing counter */
		desc_get(desc);
		mig_enqueue(usr_region, get_qreq(usr_region), desc);
		if (cnt >= 8000)  /* exhausting all descs will fail deque() */
			break;
	}

	/* multi threaded dequeue */
	start = mysecond();
	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_create(workers + i, NULL, dequeue_func, (void *)i);
		assert(ret == 0);
	}

	V("threads up & running");

	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_join(workers[i], (void **)(thread_cnts + i));
		assert(ret == 0);
	}
	end = mysecond();
	I("%s done (%.2f secs).", __func__, end - start);

	/* --- verify --- */
	I("pass. all threads' counters look fine: ");
	sum = 0;
	for (i = 0; i < NCPUS; i++ ) {
		printf("%d ", thread_cnts[i]);
		sum += thread_cnts[i];
	}
	printf("sum: %d\n", sum);
}

/* Skip enqueuing, assuming the queue is already filled up (e.g., by kernel). */
static void dequeue_test2(void)
{
	int i, ret, sum;
	double start, end;
	int thread_cnts[NCPUS] = {0};

	V("=============== %s =================", __func__);

#if 0
	freelist_init(usr_region);
	migqueue_create(usr_region, get_qreq(usr_region),
			freelist_remove(usr_region));

	/* single thread enqueue */
	cnt = 1;
	while ((desc = freelist_remove(usr_region))) {
		desc->virt_base = cnt++; /* save the increasing counter */
		mig_enqueue(usr_region, get_qreq(usr_region), desc);
	}
#endif

	/* multi threaded dequeue */
	start = mysecond();
	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_create(workers + i, NULL, dequeue_func, (void *)i);
		assert(ret == 0);
	}

	V("threads up & running");

	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_join(workers[i], (void **)(thread_cnts + i));
		assert(ret == 0);
	}
	end = mysecond();
	V("%s done (%.2f secs).", __func__, end - start);

	/* --- verify --- */
	V("pass. all threads' counters: ");
	sum = 0;
	for (i = 0; i < NCPUS; i++ ) {
		printf("%d ", thread_cnts[i]);
		sum += thread_cnts[i];
	}
	printf("sum: %d\n", sum);
}

/* -------------------------------------------------------------- */

/* idea from lfd test_queue.c queue_test_enqueuing_and_dequeuing()
 *
 * busy looping:
 * grab a desc, enqueue it (put the thread's id and the local counter in);
 * dequeue a desc immediately, check the encoded thread id and counter value.
 *
 * This case has some real multithreading complexity.
 * Multiple threads essentially communicate through the queue and interesting
 * interleavings may happen.
 *
 * e.g.
 *   Case i:
 *   thread A produces/enqueues desc1
 * 	 -->thread B dequeues/consumes desc1 (desc1 remains as queue Head)
 * 	 ...
 * 	 -->thread C dequeues/consumes desc2 and removes desc1 from the queue
 *
 *   Case ii:
 * 	 thread A produces/enqueues desc1
 * 	 -->thread B dequeues desc1 (desc1 remains as queue Head)
 * 	 -->thread C dequeues desc2 and removes desc1 from the queue
 * 	 -->thread B consumes desc1
*/

static atomic_arm_t global_cnt = 1;

static void *enqueue_dequeue_func(void *p )
{
	mig_desc *desc;
	// local counters -- what is the largest value that I've ever seen generated
	// by all threads?
	atomic_arm_t thread_cnt[NCPUS] = {0};

	int i, tid = (int)p;

	/* the # iterations cannot be too big -- it will exhaust the freelist,
	 * failing dequeue().
	 */
	for (i = 0; i < 2000; i++) {

		desc = freelist_remove(usr_region);

		if (!desc) {
			V("%d: no more desc. dequeue done. %d", tid, i);
			break;
		}

		desc->flag = tid;
		assert(global_cnt < (1 << (32 - MIG_BLOCK_ALIGN_OFFSET)) - 1); /* overflow */
		desc->virt_base =
				__sync_add_and_fetch(&global_cnt, 1);

		desc_get(desc);
//		V("%s: desc %08x refcnt %d", __func__, (uint32_t)desc, desc->refcnt);

		LFDS611_BARRIER_LOAD;

//		V("%d: enqueuing (%d)", tid, i);

		mig_enqueue(usr_region, get_qreq(usr_region), desc);

//		V("%d: dequeueing (%d)", tid, i);

		desc = mig_dequeue(usr_region, get_qreq(usr_region));
		assert(desc);

//		assert(desc->refcnt > 0); // we haven't consumed it yet!

//		if(desc->flag == 0xff) {
//			V("bug -- %d: i %d dequed desc %08x (flg %x) retired %08x",
//					tid, i, (uint32_t)desc, desc->flag, (uint32_t)tofree);
//		}

		if (!desc) {
			V("%d: no more desc. dequeue done. %d", tid, i);
			break;
		}

		assert(desc->flag < NCPUS);

		if (desc->virt_base < thread_cnt[desc->flag]) {
			V("bug: i %d read cnt %d thread cnt %lu thread %d",
				i, desc->virt_base,
				thread_cnt[desc->flag],
				desc->flag);
				assert(0);
		}

		// update the counter for a thread
		thread_cnt[desc->flag] = desc->virt_base + 1;

		desc_put(usr_region, desc);
	}

	return (void *)i;
}

static void enqueue_dequeue_test(mig_desc *base)
{
	int i, ret, sum;
	double start, end;
	int thread_cnts[NCPUS] = {0};

	V("=============== %s =================", __func__);

	freelist_init(usr_region);
	migqueue_create(usr_region, get_qreq(usr_region),
			freelist_remove(usr_region));

	start = mysecond();
	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_create(workers + i, NULL, enqueue_dequeue_func, (void *)i);
		assert(ret == 0);
	}

	V("threads up & running");

	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_join(workers[i], (void **)(thread_cnts + i));
		assert(ret == 0);
	}
	end = mysecond();
	I("%s done (%.2f secs).", __func__, end - start);

	/* --- verify --- */
	V("all threads' counters: ");
	sum = 0;
	for (i = 0; i < NCPUS; i++ ) {
		printf("%d ", thread_cnts[i]);
		sum += thread_cnts[i];
	}
	printf("sum: %d\n", sum);
}

/* ------------------------------------------------------------------------- */

/* enqueue func: busy looping enqueueing (don't touch the color).
 *
 * dequeue func: dequeuing and try to change the color once in a while.
 * verify that the dequeued node has the right color.
 * */
static void *enqueue_color_func(void *p)
{
	mig_desc *desc;
	//  double start, end;
	unsigned int i, localcnt = 0;
	color_t c;

	/* the # iterations can't be too big -- which will exhaust the freelist
	 * and thus fail dequeue()
	 */
	for (i = 0; i < 10000; i++) {
		/* grab a free desc */
		desc = freelist_remove(usr_region);
		if (!desc) {
			break;
		}

		desc_get(desc);

		/* store info into desc */
		assert(localcnt < (1 << (32 - MIG_BLOCK_ALIGN_OFFSET)) - 1); /* overflow */
		desc->virt_base = localcnt++;

		/* enqueue the desc.
		 * XXX what should we do with returned color @c? */
		c = mig_enqueue(usr_region, get_qreq(usr_region), desc);
		c = c;
//		V("enqueued color %d", c);
	}

	return NULL;
}

static void *dequeue_color_func(void *p)
{
	mig_desc *desc;
	int i, localcnt = -1;
	int j;
	color_t c = 1, ret;

	for (i = 0; i < 10000; i++) {
		for (j = 0; j < rand() %5; j++) {
			/* varying the dequeuing speed */
			desc = mig_dequeue(usr_region, get_qreq(usr_region));
			if (!desc) {
				/* queue seems to empty, try to set it to a new color */
				/* 20 colors */
				ret = migqueue_set_color_if_empty(usr_region, get_qreq(usr_region),
						(c+1) % 20);
				if (ret != -1 ) { /* coloring succeeds */
					if (ret != c) {
						E("bug: old color was %d, expected %d head idx %d",
								ret, c, get_qreq(usr_region)->head.index);
						assert(0);
					}
					c = (c+1) % 20;
				}
				break;
			} else {
				if (desc->next.color != c) { /* consistent with what we have set? */
					E("bug: color from q: %d local %d idx %d", desc->next.color,
							c, desc_to_index(usr_region, desc));
					assert(0);
				}

				// check and save to the local counter
				if (desc->virt_base <= localcnt) {
					V("bug: cnt %d localcnt %d", desc->virt_base, localcnt);
					assert(0);
				}

				localcnt = desc->virt_base;

				freelist_add(usr_region, desc);
//				desc_put(usr_region, desc);
			}
		}
	}

	V("thread done");
	return (void *)i;
}

static void coloring_test(void)
{
	int ret;

	V("=============== %s =================", __func__);

	/* two threads, one enqueues, one dequeues */
	ret = pthread_create(workers, NULL, enqueue_color_func, (void *)0);
	assert(ret == 0);
	ret = pthread_create(workers + 1, NULL, dequeue_color_func, (void *)1);
	assert(ret == 0);

	ret = pthread_join(workers[0], NULL);
	assert(ret == 0);
	ret = pthread_join(workers[1], NULL);
	if (ret != 0)
		E("worker1 return %d", ret);

	V("done");
}

/* -------------------------------------------------------------------------- */

/* use the legacy interface -- a separate debug file (works with migif.c) */
static int poke_driver_once(int cmd)
{
	int fd, ret;
	fd = open("/sys/kernel/debug/migtest", O_RDWR);
	assert(fd >= 0);

	ret = write(fd, &cmd, sizeof(int));
//	ret = ioctl(fd, cmd);
	if (ret < 0) {
		W("return value %d", ret);
	}
	close(fd);
	return ret;
}

/* use the unified debugfs file. cannot close the file */
static int poke_driver(int dev_nr, int cmd)
{
	int ret;
	int fd = configfds[dev_nr]; /* global array */

	assert(fd >= 0);

	ret = write(fd, &cmd, sizeof(int));
//	ret = ioctl(fd, cmd);
	if (ret < 0) {
		W("return value %d", ret);
	}
	return ret;
}

/* what are the numa nodes?
 * all args must be allocated */
static void print_nodes(void **ppage, int *status, int count)
{
#ifndef K2_NO_DEBUG
	int ret, i;
	bzero(status, sizeof(int)*count);

	ret = move_pages(0, /* this proc */
			count, ppage, NULL, status, 0);
	assert(ret == 0);
	V("pages on nodes -- ");
	for (i = 0; i < count; i++)
		printf("%d", status[i]);
	printf("\n");
#endif
}

/* Since SRAM may not lose content during a warm reset, we need to fill
 * random data and verify the data later.
 *
 * @return: the randomized counter initial value.
 */
static unsigned int fill_pages(void **ppage, int count)
{
	int i, j;
	void *page;
	unsigned int init, counter;

	srand(time(NULL));
	counter = init = rand();

	for (j = 0; j < count; j++) {
		page = ppage[j];
		assert(((int)page & ((1<<MIG_BLOCK_ALIGN_OFFSET) - 1)) == 0); /* align */

		for (i = 0; i < PAGE_SIZE / 4; i++)
			((unsigned int *)page)[i] = counter ++ ;
	}

	return init;
}

static unsigned int fill_huge_pages(void **ppage, int count)
{
	int i, j;
	void *page;
	unsigned int init, counter;

	srand(time(NULL));
	counter = init = rand();

	for (j = 0; j < count; j++) {
		page = ppage[j];
		assert(((int)page & ((1<<MIG_BLOCK_ALIGN_OFFSET) - 1)) == 0); /* align */

		for (i = 0; i < HUGE_PAGE_SIZE / 2; i++) 
			((unsigned int *)page)[i] = counter ++ ; 
	}

	return init;
}


static void verify_pages_content(void **ppage, int count, unsigned int init)
{
#if 1
	int i, j, err = 0;
	void *page;

	/* check mem contents */
	for (j = 0; j < count; j++) {
		page = ppage[j];

		/* fill the page */
		for (i = 0; i < PAGE_SIZE / 4; i++)
			if (((unsigned int *)page)[i] != init ++) {
#if 1
				E("err: mismatch at page %d word %d (%08x) abort", j, i,
						((unsigned int *)page)[i]);
				err = 1;
				break;
#else
				;
#endif
			}
	}

	if (!err)
		V("test pass. all data seems fine. word 128=%08x word 256=%08x",
				((unsigned int *)ppage[0])[128], ((unsigned int *)ppage[0])[256]);
#endif
}

static void verify_huge_pages_content(void **ppage, int count, unsigned int init)
{
#if 1
	int i, j, err = 0; 
	void *page;

	/* check mem contents */
	for (j = 0; j < count; j++) {
		page = ppage[j];

		/* fill the page */
		for (i = 0; i < HUGE_PAGE_SIZE / 2; i++) 
			if (((unsigned int *)page)[i] != init ++) {
#if 1
				E("err: mismatch at page %d word %d (%08x) abort", j, i,
						((unsigned int *)page)[i]);
				err = 1; 
				break;
#else
				;
#endif
			}    
	}

	if (!err)
		V("test pass. all data seems fine. word 128=%08x word 256=%08x",
				((unsigned int *)ppage[0])[128], ((unsigned int *)ppage[0])[256]);
#endif
}




/*
 * Single desc (could span multiple pages)
 *
 * use our own kernel interface to movesingle_move_test
 * use the move_pages() syscall to get the residental node.
 * move 2**order pages at a time.
 *
 * @page: src
 * @page1: dest.
 * 		if not NULL, use the address (keep both pages)
 * 		if NULL, ask the kernel to allocate new pages from @node and remap.
 *
 * caller must ensure @page has enough buffer
 */
static void single_move_test(int dev_nr, void *page, void *page1,
		int node, int order)
{
	mig_desc *desc;
	int pagecount = (1<<order);
	int i;
	void **ppage, *status, **ppage1, *pagedst, **ppagedst;
	unsigned int cnt;
	color_t color;
	double before, after;
	struct pollfd ep0_poll;
	int configfd = configfds[dev_nr]; /* global array */
	mig_region *region = regions[dev_nr];

	assert(dev_nr < NR_DEVICES);

	V("=============== %s %s =================", __func__,
			page1 ? "noremap" : "remap");
	V("page addr %08x. pagecount = %d", (uint32_t)page, pagecount);

	assert(!(page1 && node != -1));

	/* since we are going to drop lower bits across u/k if */
	assert(((int)page & ((1<<MIG_BLOCK_ALIGN_OFFSET) - 1)) == 0);

	ppage = malloc(pagecount * sizeof(void *));
	status = malloc(pagecount * sizeof(int));
	assert(ppage && status);

	for (i = 0; i < pagecount; i++)
		ppage[i] = (void *)((unsigned long)page + i * PAGE_SIZE);

	if (page1) {
		ppage1 = malloc(pagecount * sizeof(void *));
		assert(ppage1);

		for (i = 0; i < pagecount; i++)
			ppage1[i] = (void *)((unsigned long)page1 + i * PAGE_SIZE);
	}

	/* first touch the dest pages so that they actually
	 * exist */
	if (page1)
		memset(page1, 0, PAGE_SIZE * pagecount);

	cnt = fill_pages(ppage, pagecount);
	V("before -- randomized cnt is %08x", cnt);
	print_nodes(ppage, status, pagecount);

	poke_driver(dev_nr, MIG_INIT_REGION);	// init the region again

	/* --- test poll, this should timeout --- */
	ep0_poll.fd = configfd;
	ep0_poll.events = POLLIN;
#if 1
	if (poll(&ep0_poll, 1, 100) == 0)
		V("poll timeout -- okay");
	else
		I("poll okay -- bug?");
#endif

	desc = freelist_remove(region);
	assert(desc);

	before = mysecond();

	/* Set up the desc.
	 * one desc for muti pages */
	desc->virt_base = ((unsigned long)page >> MIG_BLOCK_ALIGN_OFFSET);

	if (page1)
		desc->virt_base_dest = virt_to_base(page1);
	else
		desc->node = node;
	desc->order = order;

	V("user: desc->virt_base = %08lx, desc->virt_base_dest = %08lx\n",
		desc->virt_base, desc->virt_base_dest); //hym
	V("user: desc->order = %d\n", desc->order); //hym

	mark_mig_desc_req(desc);
	D("desc.word0 %08x node %d order %d", desc->word0, desc->node, desc->order);

	/* submit the request */
	color = mig_enqueue(region, get_qreq(region), desc);
	D("enqueue. color is %d", color); color = color;

	/* kick the driver to start migration */
	if (page1)
		poke_driver(dev_nr, MIG_MOVE_SINGLE_NOREMAP);
	else
//		poke_driver(dev_nr, MIG_MOVE_SINGLE);
		poke_driver(dev_nr, MIG_MOVE_SINGLE_MIG);

	V("after");

#if 0
	/* --- time to sleep --- */
	if (poll(&ep0_poll, 1, 100) == 0)
		W("poll timeout");
	else
		V("poll okay");
#endif

//	after = mysecond();

	/* free the desc */
	i = 0;
	while (!(desc = mig_dequeue(region, &(region->qcomp)))) {
		i++;
	}
	freelist_add(region, desc);

	after = mysecond();

	I("waited %d", i);
	E("test pass: all data seem fine. %d pages %.2f MBs in %.2f ms. %.3f MB/sec",
			(1<<order), 1.0 * (1<<order)*PAGE_SIZE/1024/1024, (after - before) * 1000,
			1.0 * (1<<order)*PAGE_SIZE/1024/1024/(after-before));

	/* when noremap, dest is at a different virt addr.
	 * check its node and contents */
	if (page1) {
		pagedst = page1;
		ppagedst = ppage1;
	} else {
		pagedst = page;
		ppagedst = ppage;
	}

	print_nodes(ppagedst, status, pagecount);
	verify_pages_content(ppagedst, pagecount, cnt);

	V("test pass. all data seems fine. word 128=%08x word 256=%08x",
			((unsigned int *)pagedst)[128], ((unsigned int *)pagedst)[256]);

	V("after touch");
	print_nodes(ppagedst, status, pagecount);
}

static void * thread_pages[NR_DEVICES][2] = {0};
static int thread_node;
static int thread_order;

/* the thread wrapper */
static void *single_move_test_func(void *p)
{
	int tid = (unsigned int)p;

	/* all threads using one device */
//	single_move_test(0, thread_pages[tid][0], thread_pages[tid][1],
	/* threads use separate devices */
	single_move_test(tid, thread_pages[tid][0], thread_pages[tid][1],
			thread_node, thread_order);

	I("thread %d completed", tid);

	return NULL;
}

/* @page, page1: the base address of pages
 * @order: the # of pages each thread has.
 *
 * Since single_move_test may verify page contents, this could be slow.
 */
static void *single_move_test_mt(int nr_threads, void *page, void *page1,
		int node, int order)
{
	int i;
	int ret;
	double start, end;

	assert(nr_threads * (1 << order) <= PAGE_COUNT);

	thread_node = node;
	thread_order = order;

	start = mysecond();

	for (i = 0; i < nr_threads; i++) {
		thread_pages[i][0] = page;
		thread_pages[i][1] = page1;

		page = (void *)((unsigned long)page + (1 << order) * PAGE_SIZE);
		if (page1) {
			page1 = (void *)((unsigned long)page1 + (1 << order) * PAGE_SIZE);
		}

		ret = pthread_create(workers + i, NULL, single_move_test_func,
				(void *)i);
		assert(ret == 0);
	}

	for (i = 0; i < nr_threads; i++ ) {
		ret = pthread_join(workers[i], NULL);
		assert(ret == 0);
	}

	end = mysecond();

	E("done. %d threads. %d KB in %.2f ms. %.2f G/sec",
			nr_threads,
			nr_threads * (1 << order) * PAGE_SIZE / SZ_1K,
			(end-start)*1000,
		nr_threads * (1 << order) * PAGE_SIZE / SZ_1K / ((end-start)*1e6));
}

/* Multi descs, each of which spans one page. Kick the driver once after
 * all descs are in queue.
 *
 * passed in an array of pointers, each points to one page */
static void multiple_move_test(int dev_nr, void **ppage, int count)
{
	mig_desc *desc;
	int i, j, ret, *status;
	void *page;
	mig_region *region = regions[dev_nr];

	V("=============== %s =================", __func__);
	V("total %d pages", count);

	status = malloc(sizeof(int) * count);
	assert(status);

	poke_driver(dev_nr, MIG_INIT_REGION);

	/* what are the numa nodes? */
	ret = move_pages(0, /* this proc */
			count, ppage, NULL, status, 0);
	assert(ret == 0);
	I("before: pages on nodes -- ");
	for (i = 0; i < count; i++)
		printf("%d ", status[i]);
	printf("\n");

	for (j = 0; j < count; j++) {
		page = ppage[j];
		assert(((int)page & ((1<<MIG_BLOCK_ALIGN_OFFSET) - 1)) == 0); /* align */

		/* fill the page */
		for (i = 0; i < PAGE_SIZE / 4; i++)
			((unsigned int *)page)[i] = i;

		desc = freelist_remove(region);
		assert(desc);

		desc->virt_base = ((unsigned long)page >> MIG_BLOCK_ALIGN_OFFSET);
		desc->node = 1;
		desc->order = 0;

		V("desc->virt_base %08x", desc->virt_base);

		desc_get(desc);

		/* submit the request */
		mig_enqueue(region, get_qreq(region), desc);
	}

	/* kick the driver to start migration */
	poke_driver(dev_nr, MIG_MOVE_SINGLE);

	/* what are the numa nodes? */
	ret = move_pages(0, /* this proc */
			count, ppage, NULL, status, 0);
	assert(ret == 0);
	I("after start: pages on nodes -- ");
	for (i = 0; i < count; i++)
		printf("%d ", status[i]);
	printf("\n");

	/* check mem contents */
	for (j = 0; j < count; j++) {
		page = ppage[j];

		/* fill the page */
		for (i = 0; i < PAGE_SIZE / 4; i++)
			if (((unsigned int *)page)[i] != i) {
				E("err: mismatch at page %d word %d (%08x) abort", j, i,
						((unsigned int *)page)[i]);
				break;
			}
	}

	V("test pass: all data seem fine");

	/* what are the numa nodes? */
	ret = move_pages(0, /* this proc */
			count, ppage, NULL, status, 0);
	assert(ret == 0);
	I("after read: pages on nodes -- ");
	for (i = 0; i < count; i++)
		printf("%d ", status[i]);
	printf("\n");
}

/* Test the queue coloring mechanisms.
 *
 * Multi descs, each of which spans multiple pages.
 * Only kick the driver when the queue color shows so.
 *
 * @ppage: the array of source ptrs
 * @ppage1: that of dest ptrs. if NULL, do remap.
 *
 * @cnt: the # of descs
 * @order: 2^order pages in each desc
 *
 * Passed in an array of pointers, each points to the start of 2^order pages */
static void color_move_test(int dev_nr, void **ppage, void **ppage1,
		int count, int order)
{
	mig_desc *desc;
	int i, loop = 1;
	int j, *status, npokes = 0;
	color_t clr;
	void *page;
	unsigned int cnt;
	double before, first = 0, after;
	unsigned long bytes = count*(1<<order)*PAGE_SIZE;
	struct pollfd ep0_poll;
	int wait = 0;
	int configfd = configfds[dev_nr]; /* global array */
	mig_region *region = regions[dev_nr];

	I("=============== %s =================", __func__);
	I("total %d descs, each %d pages (total %luKB), loop %d",
			count, (1<<order), bytes/1024, loop);

	status = malloc(sizeof(int) * count);
	assert(status);

	poke_driver(dev_nr, MIG_INIT_REGION);

	D("before move");
	print_nodes(ppage, status, count);

	cnt = fill_pages(ppage, count);

	/* touch pages so that they are actually faulted in */
	for (i = 0; i < count; i++)
		memset(ppage[i], 0, PAGE_SIZE * (1<<order));
	if (ppage1) {
		for (i = 0; i < count; i++)
			memset(ppage1[i], 0, PAGE_SIZE * (1<<order));
	}
	D("after fill");

	ep0_poll.fd = configfd;
	ep0_poll.events = POLLIN;

	before = mysecond();

	for (i = 0; i < loop; i++) {
		for (j = 0; j < count; j++) {
			page = ppage[j];

//			before = mysecond();

			desc = freelist_remove(region);
			assert(desc);

			desc->virt_base = ((unsigned long)page >> MIG_BLOCK_ALIGN_OFFSET);
			if (ppage1)
				desc->virt_base_dest = virt_to_base(ppage1[j]);
			else
				desc->node = 1;
			desc->order = order;

			V("desc: virt_base %08x dest %08x", desc->virt_base,
					desc->virt_base_dest);

			/* submit the request */
			clr = mig_enqueue(region, get_qreq(region), desc);
			assert(clr != COLOR_NONE && clr > 0);
			if (clr == COLOR_USR) {
//			if (j >= count - 1 && clr == COLOR_USR) {  /* use this to test one-shot multi descs */
				if (ppage1)
					poke_driver(dev_nr, MIG_MOVE_SINGLE_NOREMAP);
				else
//					poke_driver(dev_nr, MIG_MOVE_SINGLE);
					poke_driver(dev_nr, MIG_MOVE_SINGLE_MIG);
				npokes ++;
			}

	//		if (j == 5)
	//			usleep(1000);
		}
		V("npokes %d", npokes);

		V("after move");

#if 0
		/* --- time to sleep --- */
		if (poll(&ep0_poll, 1, 100) == 0)
			W("poll timeout");
		else
			V("poll okay");
#endif

		/* dequeue completed */
		j = 0; wait = 0;
		while (1) {
			if ((desc = mig_dequeue(region, &(region->qcomp)))) {
				D("retrieved a completed desc %d/%d", j, count-1);
				/* XXX put desc back to freelist? */
				if (j==0)
					first = mysecond();
				if (++j == count)
					break;
			} else
				wait ++;
		};

		after = mysecond();

		if (wait > 0)
			W("wait %d", wait);

		/* if no remap, we switch the dest pointer to the actual one */
		if (ppage1)
			ppage = ppage1;
#if 0
		print_nodes(ppage, status, count);
		verify_pages_content(ppage, count, cnt);
#endif
	}

//	after = mysecond();

	E("%s test pass: total %d pages (in %d req) pokes %d "
			"%.3f ms (first %.3f ms). %.3f MB/sec",
			ppage1 ? "rep" : "mig",
			count * (1 << order), count, npokes,
			1e3 *(after - before), 1e3*(first - before),
			loop * 1.0 * bytes /1024/1024/(after-before));
	print_nodes(ppage, status, count);
}

/* return: error code */
static int kernel_driver_test(void)
{
	int j;
	char * address = NULL;
	const char * fns[] = {
			"/sys/kernel/debug/migif",
			"/sys/kernel/debug/migif1",
			"/sys/kernel/debug/migif2",
			"/sys/kernel/debug/migif3"
	};

	for (j = 0; j < NR_DEVICES; j++) {
		configfds[j] = open(fns[j], O_RDWR);
		if(configfds[j] < 0) {
			perror("open");
			return -1;
		}

		address = mmap(NULL, PAGE_COUNT * PAGE_SIZE, PROT_READ|PROT_WRITE,
				MAP_SHARED, configfds[j], 0);
		if (address == MAP_FAILED) {
			perror("mmap");
			return -1;
		}

		/* now the driver should have d/s ready; directly use them */
		regions[j] = (mig_region *)address;
		I("dev%d: mmap'd region is %08x ndescs %d",
				j, (uint32_t)regions[j], regions[j]->ndescs);
	}

	return 0;
}

/*
 * Test entries. The tests can be turned on and off individually.
 */

int main(int argc, char **argv)
{
	/* user mmap'd buffers. used as user's migregion and later data buffers */
	void *p, *p1;
	void **pp, **pp1 = NULL;

	int i, ret, order;

	/* basic check */
	assert(NCPUS < (1 << MIG_BLOCK_ALIGN_OFFSET)); // we overload this field in testing
	V("sizeof(desc)=%d sizeof(mig_desc_ptr)=%d", sizeof(mig_desc),
			sizeof(mig_desc_ptr));

	p = alloc_memory(PAGE_COUNT);
	assert(p);

#if 0
	usr_region = init_migregion(p, PAGE_SIZE * PAGE_COUNT);
	assert(usr_region->ndescs < (1 << (8 * sizeof(index_t))));
	V("ndescs %d", region->ndescs);

	/* ------------ userspace test ------------ */
	{
		mig_desc *base;

		base = get_desc_array(usr_region);
		freelist_stress();
		enqueue_test(base);
		dequeue_test(base);
		enqueue_dequeue_test(base);

		usr_region = init_migregion(p, SZ_4K * PAGE_COUNT);
		coloring_test();

		// XXX test scalability
		// XXX unmap memory
	}
#endif

	// switch @region to use the mig driver's ...
	kernel_driver_test();

	/* ------------ user uses region init'd by kernel ------------ */
#if 0
	assert(usr_region);
	base = get_desc_array(usr_region);
	freelist_stress();
	enqueue_test(base);
	dequeue_test(base);
	enqueue_dequeue_test(base);
#endif

	/* ------------ kernel enqueue then user dequeue ------------ */
#if 0
	assert(usr_region);
	poke_driver(0, MIG_INIT_REGION);
	poke_driver(0, MIG_FILL_QREQ);
	dequeue_test2();

	poke_driver(0, MIG_INIT_REGION);	// init the region again
	enqueue_test2();
	poke_driver(0, MIG_EMPTY_QREQ);	// ask driver to empty the queue

	if (MIG_BLOCK_ROUND_UP(p) -
			(unsigned long)p - PAGE_SIZE * PAGE_COUNT < PAGE_SIZE) {
		E("cannot find a BLOCK_SIZE aligned vaddr");
		goto cleanup;
	}
#endif


	/* one desc, one or more pages
	 * order: 4--16 pages     7--128 pages
	 * */
	/* remap */
	order = 4;

	// not maintained so buggy? use the following
//	single_move_test(0, (void *)MIG_BLOCK_ROUND_UP(p), NULL, 1, order);

	single_move_test_mt(1, p, NULL, 1, order);

#if 0
	/* noremap */
	p1 = numa_alloc_onnode((1<<order)*PAGE_SIZE, 1);
	V("mem on node1. vaddr %08lx", (unsigned long)p1);
	assert(p1);
	single_move_test(0, p, p1, -1, order);

	/* back-to-back migration is likely to fail as the page may be in
	 * pagevec (not on any LRU and thus -EBUSY)
	 */
//	single_move_test(0, (void *)MIG_BLOCK_ROUND_UP(p), 0);
#endif

#if 0 /* obsoleted */
	/* one shot, multi pages */
	assert(PAGE_COUNT >= 8);
	pp = malloc(sizeof(void *) * 8);
	assert(pp);
	for (i = 0; i < 8; i++) {
		pp[i] = (void *)((uint32_t)p + i * PAGE_SIZE);
	}
	multiple_move_test(0, pp, 8);
	free(pp);
#endif

#if 0
	/* color test  -- remap (slow)
	 * multi shots, multi pages */
	order = 4;
	assert(PAGE_COUNT >= 8 * (1 << order));
	pp = malloc(sizeof(void *) * 8);
	assert(pp);
	for (i = 0; i < 8; i++) {
		pp[i] = (void *)((uint32_t)p + i * PAGE_SIZE * (1 << order));
	}
	color_move_test(0, pp, NULL, 8, order);
	free(pp);
#endif

#if 0
	/* color test + sep virt addr (fast) */
	order = 7;
	p1 = numa_alloc_onnode(8 * (1<<order) * PAGE_SIZE, 1);
	I("mem on node1. vaddr %08lx", (unsigned long)p1);
	assert(p1);

	assert(PAGE_COUNT >= 8 * (1 << order));
	pp = malloc(sizeof(void *) * 8);
	pp1 = malloc(sizeof(void *) * 8);

	assert(pp);
	for (i = 0; i < 8; i++) {
		pp[i] = (void *)((uint32_t)p + i * PAGE_SIZE * (1 << order));
		pp1[i] = (void *)((uint32_t)p1 + i * PAGE_SIZE * (1 << order));
		V("save %08x...", (unsigned int)pp1[i]);
	}

	color_move_test(0, pp, pp1, 8, order);
	free(pp);
	free(pp1);
#endif

#if 1
cleanup:
	/* ---------- cleanup driver ----------- */
	ret = munmap(p, PAGE_COUNT * PAGE_SIZE);
	assert(!ret);
	if (usr_region && usr_region != p) {
		ret = munmap(usr_region, PAGE_COUNT * PAGE_SIZE);
		assert(!ret);
	}

	for (i = 0; i < NR_DEVICES; i++)
		close(configfds[i]);

#endif

	return 0;
}

