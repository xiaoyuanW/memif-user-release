/*
 * runtime.c
 *
 * The minimal runtime for streaming workloads -- built atop memif.
 *
 *  Created on: Aug 6, 2015
 *      Author: xzl
 */

#define _BSD_SOURCE
#define K2_NO_DEBUG 1
//#define K2_NO_MEASUREMENT 1

# include <stdio.h>
# include <unistd.h>
# include <math.h>
# include <float.h>
# include <limits.h>
# include <sys/time.h>

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

#include "word.h"
#include "measure.h"
#include "log-xzl.h"
#include "mig.h"
#include "misc-xzl.h"
#include "clearcache.h"

#include "threadpool/src/threadpool.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE SZ_4K
#endif

#define NCPUS 4

#ifndef STREAM_TYPE
#define STREAM_TYPE double
#endif

/* msmc buffer size */
//#define PAGE_ORDER 7 	/* 128 pages, 512KB */
//#define PAGE_ORDER 8 	/* 256 pages, 1M */
//#define PAGE_ORDER 9 	/* 512 pages, 2M */
#define PAGE_ORDER 10 	/* 1024 pages, 4M */
#define PAGE_COUNT (1<<PAGE_ORDER)

//#define DESC_ORDER 5  // 32 pgs
//#define DESC_ORDER 6  // 64 pgs, 256K
#define DESC_ORDER 7  // 128 pgs, 512K   -- the max desc we can support.

#define NUM_DESCS (1 << (PAGE_ORDER - DESC_ORDER))
#define DESC_SIZE ((1<<DESC_ORDER) * PAGE_SIZE) /* single desc, in bytes */

/* 40 -- traid: 57000 vs 47200
 * 40 -- dist: 106295 vs 77064
 * */
//#define DDR_FACTOR	2
#define DDR_FACTOR	80   /* ddrsize / msmcsize */

static int fd;
static struct pollfd the_ep;
static char *ddr = 0, *msmc = 0;
static mig_region *region = NULL;
static unsigned long msmc_size;
static unsigned long ddr_size;

static unsigned int the_ddri = 0;  /* shared. which ddr buffer to consume next? */
static unsigned int the_consumed = 0;

/* --- thread pool stuffs --- */
static threadpool_t *pool;
//#define THREAD NCPUS
#define THREAD 4
#define QUEUE  32

static void check_ddr_addr(void *p)
{
	char *pp = (char *)p;
	if (pp < ddr && pp >= ddr+ddr_size) {
		E("ddr out of range");
		assert(0);
	}
}

static void check_msmc_addr(void *p)
{
	char *pp = (char *)p;
	if (pp < msmc && pp >= msmc+msmc_size) {
		E("msmc out of range");
		assert(0);
	}
}

static int poke_driver(int fd, int cmd)
{
	int ret;
	assert(fd >= 0);

	ret = write(fd, &cmd, sizeof(int));
//	ret = ioctl(fd, cmd);
	if (ret < 0) {
		W("return value %d", ret);
	}
	return ret;
}

/* --------------------------------------------- */

typedef int consumer_func(char *, int);

/*
 * note: see comments in @consumer_traid_omp()
 */
static int consumer_traid(char *buf, int size)
{
	STREAM_TYPE *aa, *bb, *cc;
	int j;
//	STREAM_TYPE scalar = 3.0;
	int BytesPerWord = sizeof(STREAM_TYPE);
	int sz = ((size / 3) & ~(BytesPerWord-1)); 	/* 4-byte aligned addr */

	/* don't ask for (size % 3 == 0).
	 * we wouldn't go over the bound anyway.
	 */

	aa = (STREAM_TYPE *)buf;
	bb = (STREAM_TYPE *)(buf + sz);
	cc = (STREAM_TYPE *)(buf + 2 * sz);

	/* no parallel for */
	for (j = 0; j < sz/BytesPerWord; j++)
		aa[j] = bb[j] + 3.0 * cc[j];

	return 0;
}

static int consumer_traid_omp(char *buf, int size)
{
	STREAM_TYPE *aa, *bb, *cc;
	int j;
//	STREAM_TYPE scalar = 3.0;
	int BytesPerWord = sizeof(STREAM_TYPE);
	int sz = ((size / 3) & ~(BytesPerWord-1)); 	/* 4-byte aligned addr */

	aa = (STREAM_TYPE *)buf;
	bb = (STREAM_TYPE *)(buf + sz);
	cc = (STREAM_TYPE *)(buf + 2 * sz);

	#pragma omp parallel for
	for (j = 0; j < sz/BytesPerWord; j++)
		/*
		 * Note: it is strange that if we use @scalar here,
		 * we will see an over-optimal performance, even higher
		 * than the add case. this looks wrong but we didn't have
		 * time to pursue.
		 *
		 * Putting a constant here seems to make it gone.
		 */
		aa[j] = bb[j] + 3.0 * cc[j];

	return 0;
}

static int consumer_add_omp(char *buf, int size)
{
	STREAM_TYPE *aa, *bb, *cc;
	int j;
	int BytesPerWord = sizeof(STREAM_TYPE);
	int sz = ((size / 3) & ~(BytesPerWord-1)); 	/* 4-byte aligned addr */

	/* don't ask for (size % 3 == 0).
	 * we wouldn't go over the bound anyway.
	 */

	aa = (STREAM_TYPE *)buf;
	bb = (STREAM_TYPE *)(buf + sz);
	cc = (STREAM_TYPE *)(buf + 2 * sz);

	#pragma omp parallel for
	for (j = 0; j < sz/BytesPerWord; j++)
		aa[j] = bb[j] + cc[j];

	return 0;
}

/* --------------------------------- */

/* Write the result to ddr, instead of msmc.
 * This is because we don't see good performance from @run_readahead_wb().
 * See its comments for more details.
 *
 * Note that we directly use the global DDR buffer. We also do a random access
 * to match the behavior of @consume_whole_ddr().
 *
 * See comments in @consumer_traid_omp() for @scalar. */
static int consumer_traid_wt(char *buf, int size)
{
	STREAM_TYPE *aa, *bb, *cc;
	int j;
//	STREAM_TYPE scalar = 3.0;
	int BytesPerWord = sizeof(STREAM_TYPE);
	int sz = ((size / 3) & ~(BytesPerWord-1)); 	/* 4-byte aligned addr */

	static int i = 0; /* counter */
	static int k = 0; /* the position of ddr buf */

	/* don't ask for (size % 3 == 0).
	 * we wouldn't go over the bound anyway.
	 */
	k = (k + i) % (NUM_DESCS * DDR_FACTOR);
	i ++;

	aa = (STREAM_TYPE *)((char *)ddr + k * DESC_SIZE);
	bb = (STREAM_TYPE *)(buf + sz);
	cc = (STREAM_TYPE *)(buf + 2 * sz);

	#pragma omp parallel for
	for (j = 0; j < sz/BytesPerWord; j++)
		aa[j] = bb[j] + 3.0 * cc[j];

	return 0;
}

static int consumer_add_wt(char *buf, int size)
{
	STREAM_TYPE *aa, *bb, *cc;
	int j;
	int BytesPerWord = sizeof(STREAM_TYPE);
	int sz = ((size / 3) & ~(BytesPerWord-1)); 	/* 4-byte aligned addr */

	static int i = 0; /* counter */
	static int k = 0; /* the position of ddr buf */

	/* don't ask for (size % 3 == 0).
	 * we wouldn't go over the bound anyway.
	 */
	k = (k + i) % (NUM_DESCS * DDR_FACTOR);
	i ++;

	aa = (STREAM_TYPE *)((char *)ddr + k * DESC_SIZE);
	bb = (STREAM_TYPE *)(buf + sz);
	cc = (STREAM_TYPE *)(buf + 2 * sz);

	#pragma omp parallel for
	for (j = 0; j < sz/BytesPerWord; j++)
		aa[j] = bb[j] + cc[j];

	return 0;
}

/* --------------------------------- */

/* the kernel of streamcluster. see its @pgain() */
static int consumer_dist_omp(char *buf, int size)
{
	STREAM_TYPE *aa, *bb;
	volatile STREAM_TYPE res = 0;
	int j;
	int BytesPerWord = sizeof(STREAM_TYPE);
	int sz = ((size / 2) & ~(BytesPerWord-1)); 	/* 4-byte aligned addr */

	/* don't ask for (size % 3 == 0).
	 * we wouldn't go over the bound anyway.
	 */

	aa = (STREAM_TYPE *)buf;
	bb = (STREAM_TYPE *)(buf + sz);

	#pragma omp parallel for
	for (j = 0; j < sz/BytesPerWord; j++)
		res += (bb[j] - aa[j]) * (bb[j] - aa[j]);
}

/* --------------------------------- */


static int cmpfunc (const void * a, const void * b)
{
   return ( *(float*)a - *(float*)b );
}

static void consumer_sort(char *buf, int size)
{
	int BytesPerWord = sizeof(STREAM_TYPE);
	int count = size / BytesPerWord;

	qsort(buf, count, BytesPerWord, cmpfunc);
}

/* --------------------------------- */

/* a place to run consumer once */
static void consume_single(void)
{
	k2_measure("start");
	consumer_traid((char *)msmc, DESC_SIZE);
	k2_measure("msmc-consumed");
	consumer_traid((char *)ddr, DESC_SIZE);
	k2_measure("ddr-consumed");
	k2_flush_measure_format();

	clearcache((unsigned char *)ddr, DESC_SIZE);
	clearcache((unsigned char *)msmc, DESC_SIZE);
}

static void consume_whole_ddr(consumer_func consumer)
{
	int i, j;
	clearcache((unsigned char *)ddr, ddr_size);

	/* to make a fair comparison, we want to avoid purely seq access
	 * and instead consume memory in chunks. this is also the case of
	 * streamcluster where points are shuffled. */
	k2_measure("start");
	for (j = 0, i = 0; i < NUM_DESCS * DDR_FACTOR; i++) {
		j = (j + i) % (NUM_DESCS * DDR_FACTOR);
//		E("%d", j);
		consumer((char *)ddr + j * DESC_SIZE, DESC_SIZE);
//		k2_measure("ddr-consumed-one");
	}
	k2_measure("whole-ddr-done");
	k2_flush_measure_format();

	clearcache((unsigned char *)ddr, ddr_size);
	E("done. that's %d descs (%d KB each. msmc x%d ddr x%d)",
			NUM_DESCS * DDR_FACTOR, DESC_SIZE/1024, NUM_DESCS,
			NUM_DESCS*DDR_FACTOR);
}

/* a place to test moving single desc */
static void move_single(void)
{
	int i;
	mig_desc *desc;

	k2_measure("start");
	for (i = 0; i < 8; i ++)
		memif_start_replication(fd, region, ddr, msmc, DESC_ORDER);

	for (i = 0; i < 8; i ++) {
		while (!(desc = mig_dequeue(region, &(region->qcomp))))
			;
//		if (poll(&the_ep, 1, 1) == 0)  /* not timeout */
//			W("timeout");
		k2_measure("moved-one");
	}
	k2_flush_measure_format();
}

void dummy_task(void *arg)
{
	I("I am supposed to consume a buf");
}

void task(void *arg)
{
	mig_desc *desc = (mig_desc *)arg;
	unsigned int ddri;
	void *msmc_buf;

again:
	assert(desc);

	msmc_buf = (void *)base_to_virt(desc->virt_base_dest);
	I("desc %08lx vaddr %08lx", (unsigned long)desc, (unsigned long)msmc_buf);
	freelist_add(region, desc);

	/* consume here... */

	/* done with a msmc buffer. start a new move */
	ddri = __sync_fetch_and_add(&the_ddri, 1);
	I("ddri %d", ddri);
	if (ddri >= NUM_DESCS * DDR_FACTOR) {
		I("ddri %d. quit", ddri);
		return; /* done. all input consumed */
	}

	I("mov %d: ddr %08lx -> msmc %08lx", ddri,
			(unsigned long)ddr + msmc_size / NUM_DESCS * ddri,
			(unsigned long)msmc_buf);

//	sleep(1);
	I("going to move");

	check_ddr_addr(ddr + msmc_size / NUM_DESCS * ddri);
	check_msmc_addr(msmc_buf);

	memif_start_replication(fd, region, ddr + msmc_size / NUM_DESCS * ddri,
		        			msmc_buf, DESC_ORDER);

	/* more task can be done w/o sleep? */
	if ((desc = mig_dequeue(region, &(region->qcomp))))
		goto again;

	/* nothing for us to do. */

	/* spin wait? */

#if 1
	/* sleep? */
	while (!(desc = mig_dequeue(region, &(region->qcomp)))) {
		if (ddri >= NUM_DESCS * DDR_FACTOR)
			return;
	}
	goto again;
#endif
}

static void *pthread_task(void *p)
{
	task(p);
	return NULL;
}

/*
 * Not for thread pool. independent
 *
 * Be careful: don't finish until all move work is done (i.e. the kernel
 * thread's work is done).
 *
 * otherwise kernel thread will see invalid mm/vma -- crash.
 */
static void *pthread_task1(void *p)
{
	mig_desc *desc;
	unsigned int ddri;
	void *msmc_buf;
	int pid = (int)p;
	int consumed;

	while (1) {
		/* peek -- may save one CAS */
		if (the_consumed >= NUM_DESCS * DDR_FACTOR)
			goto done;

		while (1) {
			if (poll(&the_ep, 1, 1) != 0) { /* not timeout */
				if (desc = mig_dequeue(region, &(region->qcomp)))
					break;
			}

			if (the_consumed >= NUM_DESCS * DDR_FACTOR) {
				I("the_consumed %d. quit", the_consumed);
				goto done;
			}
		}
#if 0
		while (!(desc = mig_dequeue(region, &(region->qcomp)))) {
			/* don't stop trying until all bufs consumed */
			if (the_consumed >= NUM_DESCS * DDR_FACTOR) {
				I("the_consumed %d. quit", the_consumed);
				goto done;
			}
		}
#endif

//		if (pid == 0)
			k2_measure("got-onebuf");

		msmc_buf = (void *)base_to_virt(desc->virt_base_dest);
		I("desc %08lx vaddr %08lx", (unsigned long)desc, (unsigned long)msmc_buf);
		freelist_add(region, desc);

		/* signal other threads asap */
		consumed = __sync_add_and_fetch(&the_consumed, 1);

#if 0
		/* consume here... */
		consumer_traid(msmc_buf, DESC_SIZE);

		if (pid == 0)
			k2_measure("consume-one");
		if (consumed == NUM_DESCS * DDR_FACTOR)
			k2_measure("all-done");
#endif

		/* we want to peek the global (updated) value... */
		if (the_consumed >= NUM_DESCS * DDR_FACTOR) {
			/* we've consumed all buffers. also no need to move. done */
			goto done;
		}

		/* a msmc buffer is just consumed & free now.
		 * start a new move into it */
		if (the_ddri >= NUM_DESCS * DDR_FACTOR) /* peek it */
			continue;
		ddri = __sync_fetch_and_add(&the_ddri, 1);
		if (ddri >= NUM_DESCS * DDR_FACTOR) /* no new ddr bufs */
			continue;

		I("mov %d: ddr %08lx -> msmc %08lx", ddri,
				(unsigned long)ddr + msmc_size / NUM_DESCS * ddri,
				(unsigned long)msmc_buf);

		check_ddr_addr(ddr + msmc_size / NUM_DESCS * ddri);
		check_msmc_addr(msmc_buf);

		memif_start_replication(fd, region, ddr + msmc_size / NUM_DESCS * ddri,
								msmc_buf, DESC_ORDER);
		if (pid == 0)
			k2_measure("move-one");
	}

done:
	if (pid == 0)
		k2_measure("threads-exit");

	return NULL;
}

static void run(void)
{
	struct pollfd ep0_poll;
	int i = 0, wait = 0;
	mig_desc *desc = NULL;

	the_ddri = 0;

	/* fill all buffers */
	for (i = 0; i < NUM_DESCS; i++) {

		check_ddr_addr(ddr + msmc_size / NUM_DESCS * the_ddri);
		check_msmc_addr(msmc + msmc_size / NUM_DESCS * i);

		memif_start_replication(fd, region, ddr + msmc_size / NUM_DESCS * the_ddri,
	        			msmc + msmc_size / NUM_DESCS * i, DESC_ORDER);
		the_ddri ++;
	}

	i = 0;

	while (1) {
		if (i >= NUM_DESCS || i >= THREAD)
			break;

		while (!(desc = mig_dequeue(region, &(region->qcomp))))
			wait++;

		threadpool_add(pool, &task, (void *)desc, 0);
		i ++;
	}

//	sleep(1);
	assert(threadpool_destroy(pool, threadpool_graceful) == 0);

	I("done. wait %d", wait);
}

static void run1(void)
{
	struct pollfd ep0_poll;
	int i = 0, wait = 0;
	mig_desc *desc = NULL;

	the_ddri = 0;

	/* fill all buffers */
	for (i = 0; i < NUM_DESCS; i++) {

		check_ddr_addr(ddr + msmc_size / NUM_DESCS * the_ddri);
		check_msmc_addr(msmc + msmc_size / NUM_DESCS * i);

		memif_start_replication(fd, region, ddr + msmc_size / NUM_DESCS * the_ddri,
	        			msmc + msmc_size / NUM_DESCS * i, DESC_ORDER);
		the_ddri ++;
	}


	while (!(desc = mig_dequeue(region, &(region->qcomp))))
		wait++;

	threadpool_add(pool, &task, (void *)desc, 0);

//	sleep(1);
	assert(threadpool_destroy(pool, threadpool_graceful) == 0);

	I("done. wait %d", wait);
}

/* use the threadpool worker function for thread */
static void run_pthread(void)
{
	int i = 0, wait = 0;
	mig_desc *desc = NULL;
	pthread_t th;
	int ret;

	the_ddri = 0;

	/* fill all buffers */
	for (i = 0; i < NUM_DESCS; i++) {

		check_ddr_addr(ddr + msmc_size / NUM_DESCS * the_ddri);
		check_msmc_addr(msmc + msmc_size / NUM_DESCS * i);

		memif_start_replication(fd, region, ddr + msmc_size / NUM_DESCS * the_ddri,
	        			msmc + msmc_size / NUM_DESCS * i, DESC_ORDER);
		the_ddri ++;
	}

	while (!(desc = mig_dequeue(region, &(region->qcomp))))
		wait++;

	ret = pthread_create(&th, NULL, pthread_task, (void *)desc);
	assert(ret == 0);

//	sleep(1);
	ret = pthread_join(th, NULL);
	assert(ret == 0);

	I("done. wait %d", wait);
}

/* launcher of pure pthread workers */
static void run_pthread1(void)
{
	struct pollfd ep0_poll;
	int i = 0, wait = 0;
	mig_desc *desc = NULL;
	int ret;
	pthread_t workers[4];

	the_ddri = 0;
	the_consumed = 0;

	k2_measure("start");

	/* start move to all buffers */
	for (i = 0; i < NUM_DESCS; i++) {

		check_ddr_addr(ddr + msmc_size / NUM_DESCS * the_ddri);
		check_msmc_addr(msmc + msmc_size / NUM_DESCS * i);

		memif_start_replication(fd, region, ddr + msmc_size / NUM_DESCS * the_ddri,
	        			msmc + msmc_size / NUM_DESCS * i, DESC_ORDER);
		the_ddri ++;
	}

	k2_measure("init-mv-started");

	/* now let worker threads to compete & spin ... */
	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_create(workers + i, NULL, pthread_task1, (void *)i);
		assert(ret == 0);
	}

	for (i = 0; i < NCPUS; i++ ) {
		ret = pthread_join(workers[i], NULL);
		assert(ret == 0);
	}
	k2_measure("done.");
	k2_flush_measure_format();
//	sleep(1);

	I("done. wait %d", wait);
}

/* single thread, readahead.
 * use openmp to consume buffers. */
static void run_readahead(consumer_func consumer)
{
	int i = 0, wait = 0;
	mig_desc *desc = NULL;
	void *msmc_buf = 0;
	struct pollfd ep0_poll;

	the_ddri = 0;
	the_consumed = 0;

	ep0_poll.fd = fd;
	ep0_poll.events = POLLIN;

	k2_measure("start");

	/* fill the move pipeline */
	for (i = 0; i < NUM_DESCS; i++) {

		check_ddr_addr(ddr + msmc_size / NUM_DESCS * the_ddri);
		check_msmc_addr(msmc + msmc_size / NUM_DESCS * i);

		memif_start_replication(fd, region, ddr + msmc_size / NUM_DESCS * the_ddri,
	        			msmc + msmc_size / NUM_DESCS * i, DESC_ORDER);
		I("issued %d/%d", the_ddri, NUM_DESCS * DDR_FACTOR-1);
		the_ddri ++;
	}

//	k2_measure("init-mv-started");

	while (1) {
		while (!(desc = mig_dequeue(region, &(region->qcomp)))) {
			/* msmc not ready yet. keep cpu busy */
			if (the_ddri < NUM_DESCS * DDR_FACTOR) {
				consumer(ddr + msmc_size / NUM_DESCS * the_ddri,
					DESC_SIZE);
				the_ddri ++;
//				k2_measure(">> consume-ddr-one");
				if (++the_consumed == NUM_DESCS * DDR_FACTOR)
					goto done;
			}
		}
//		k2_measure("* dma-wait-end");

		msmc_buf = (void *)base_to_virt(desc->virt_base_dest);
		freelist_add(region, desc);

		consumer(msmc_buf, DESC_SIZE);
//		k2_measure("> consume-msmc-one");

		I("consume msmc %d", the_consumed);

		if (++the_consumed == NUM_DESCS * DDR_FACTOR)
			break;

		if (the_ddri < NUM_DESCS * DDR_FACTOR) {
			memif_start_replication(fd, region,
									ddr + msmc_size / NUM_DESCS * the_ddri,
				        			msmc_buf, DESC_ORDER);
			V("issued %d/%d", the_ddri, NUM_DESCS * DDR_FACTOR-1);
			the_ddri ++;
//			k2_measure("issue-end");
		}
	}

done:
	k2_measure("done.");
	k2_flush_measure_format();
	E("done. that's %d descs (%d KB each. msmc x%d ddr x%d)",
			NUM_DESCS * DDR_FACTOR, DESC_SIZE/1024, NUM_DESCS,
			NUM_DESCS*DDR_FACTOR);
}

/* Same as runahead, but wb 1/2 buf to ddr. (In fact, STREAM only needs to
 * write back 1/3).
 *
 * This works correctly, but does not yield good performance, probably because
 * the wb traffic slows down the "move-in" traffic and blocks CPU from
 * accessing msmc.
 *
 * Further investigation is possible by turning all k2_measure() points to test
 * small cases.
 *
 * */
static void run_readahead_wb(consumer_func consumer)
{
	int i = 0, wait = 0;
	mig_desc *desc = NULL;
	void *msmc_buf = 0;
	struct pollfd ep0_poll;

	the_ddri = 0;
	the_consumed = 0; /* only increase this after wb */

	ep0_poll.fd = fd;
	ep0_poll.events = POLLIN;

	k2_measure("start");

	/* fill the move pipeline */
	for (i = 0; i < NUM_DESCS; i++) {

		check_ddr_addr(ddr + msmc_size / NUM_DESCS * the_ddri);
		check_msmc_addr(msmc + msmc_size / NUM_DESCS * i);

		memif_start_replication(fd, region, ddr + msmc_size / NUM_DESCS * the_ddri,
	        			msmc + msmc_size / NUM_DESCS * i, DESC_ORDER);
		I("issued %d/%d", the_ddri, NUM_DESCS * DDR_FACTOR-1);
		the_ddri ++;
	}

//	k2_measure("init-mv-started");

	while (1) {
		while (!(desc = mig_dequeue(region, &(region->qcomp)))) {
			/* no msmc buf available. consume ddr. */
			if (the_ddri < NUM_DESCS * DDR_FACTOR) {
				consumer(ddr + msmc_size / NUM_DESCS * the_ddri,
					DESC_SIZE);
				the_ddri ++;
//				k2_measure(">> consume-ddr-one");
				I("ddr wb %d", the_consumed);
				if (++the_consumed == NUM_DESCS * DDR_FACTOR)
					goto done;
			}
		}
//		k2_measure("* dma-wait-end");

		/* a move is completed */
		msmc_buf = (void *)base_to_virt(desc->virt_base_dest);
		if (desc->order < DESC_ORDER) {
			/* a wb move is done. now the whole buf can be reused. */
			I("msmc wb %d", the_consumed);

			if (++the_consumed == NUM_DESCS * DDR_FACTOR) {
				/* are we the last one? */
				freelist_add(region, desc);
				goto done;
			}

			if (the_ddri < NUM_DESCS * DDR_FACTOR) {
				memif_start_replication(fd, region,
										ddr + msmc_size / NUM_DESCS * the_ddri,
					        			msmc_buf, DESC_ORDER);
				I("issued %d/%d", the_ddri, NUM_DESCS * DDR_FACTOR-1);
				the_ddri ++;
	//			k2_measure("issue-end");
			}
		} else {
			/* a move-in is done. we have a new buf to consume */
			consumer(msmc_buf, DESC_SIZE);
//			k2_measure("> consume-msmc-one");

			/* after consuming, wb 1/2 (should be 1/3) buffer to ddr.
			 * XXX we use a dummy ddr addr not a real one... */
			memif_start_replication(fd, region,
									msmc_buf,
									ddr,
									DESC_ORDER - 2);
	//		k2_measure("wb-start");
		}

		freelist_add(region, desc);
	}

done:
	k2_measure("done.");
	k2_flush_measure_format();
	E("done. that's %d descs (%d KB each. msmc x%d ddr x%d)",
			NUM_DESCS * DDR_FACTOR, DESC_SIZE/1024, NUM_DESCS,
			NUM_DESCS*DDR_FACTOR);
}

static void init(void)
{
	int BytesPerWord = sizeof(STREAM_TYPE);

	/* --- memif init ---- */
	fd = memif_open(0, &region);
	assert(fd > 0);

	poke_driver(fd, MIG_INIT_REGION);

	the_ep.fd = fd;
	the_ep.events = POLLIN;

	/* --- buffer init --- */
	assert(PAGE_ORDER > DESC_ORDER);

	msmc_size = NUM_DESCS * DESC_SIZE;
	ddr_size =  DDR_FACTOR * msmc_size;

	ddr = numa_alloc_onnode(ddr_size, 0);
//	ddr = malloc(ddr_size);
//	ddr = mmap(0, ddr_size, (PROT_READ|PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	msmc = numa_alloc_onnode(msmc_size, 1);

	assert(msmc && ddr);
	I("mem on ddr(node0). vaddr %08lx -- %08lx", (unsigned long)ddr,
				(unsigned long)ddr + ddr_size);
	I("mem on msmc(node1). vaddr %08lx -- %08lx", (unsigned long)msmc,
			(unsigned long)msmc + msmc_size);

	/* touch all words */
	memset(msmc, 0, msmc_size);
	memset(ddr, 0, ddr_size);

	I("num_descs %d, each %d KB (%d pages %d STREAM elements)",
			NUM_DESCS, DESC_SIZE/1024,
			(1<<DESC_ORDER), DESC_SIZE/BytesPerWord);

	/* drop all cache */
	clearcache((unsigned char *)ddr, ddr_size);
	clearcache((unsigned char *)msmc, msmc_size);

	/* ---- thread pool ---- */
	assert((pool = threadpool_create(THREAD, QUEUE, 0)) != NULL);
	I("Pool started with %d threads and "
	            "queue size of %d\n", THREAD, QUEUE);
}

static void cleanup(void)
{
	int ret;

	numa_free(ddr, ddr_size);
	numa_free(msmc, msmc_size);

	ret = munmap(region, 3 * PAGE_SIZE);
	assert(!ret);

	/* close memif XXX */
	close(fd);

	W("clean up done");
}

int main(int argc, char **argv)
{
	init();

//	consume_single();

//	move_single();

	/* available wb consumers:
	 *
	 * consumer_traid_omp()
	 * consumer_traid()
	 *
	 * available no wb consumers:
	 * consumer_dist_omp()
	 */

//	consume_whole_ddr(consumer_dist_omp);
//	run_readahead(consumer_dist_omp);

//	consume_whole_ddr(consumer_traid_omp);
//	run_readahead(consumer_traid_wt);
//	run_readahead_wb(consumer_traid_omp); /* see its comments */

	consume_whole_ddr(consumer_add_omp);
	run_readahead(consumer_add_wt);

//	run_readahead(consumer_traid_omp);

//	run_pthread();
//	run_pthread1();
//	run1();
//	run();


	cleanup();
	return 0;
}
