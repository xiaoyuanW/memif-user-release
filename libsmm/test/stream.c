/*-----------------------------------------------------------------------*/
/* Program: STREAM                                                       */
/* Revision: $Id: stream.c,v 5.10 2013/01/17 16:01:06 mccalpin Exp mccalpin $ */
/* Original code developed by John D. McCalpin                           */
/* Programmers: John D. McCalpin                                         */
/*              Joe R. Zagar                                             */
/*                                                                       */
/* This program measures memory transfer rates in MB/s for simple        */
/* computational kernels coded in C.                                     */
/*-----------------------------------------------------------------------*/
/* Copyright 1991-2013: John D. McCalpin                                 */
/*-----------------------------------------------------------------------*/
/* License:                                                              */
/*  1. You are free to use this program and/or to redistribute           */
/*     this program.                                                     */
/*  2. You are free to modify this program for your own use,             */
/*     including commercial use, subject to the publication              */
/*     restrictions in item 3.                                           */
/*  3. You are free to publish results obtained from running this        */
/*     program, or from works that you derive from this program,         */
/*     with the following limitations:                                   */
/*     3a. In order to be referred to as "STREAM benchmark results",     */
/*         published results must be in conformance to the STREAM        */
/*         Run Rules, (briefly reviewed below) published at              */
/*         http://www.cs.virginia.edu/stream/ref.html                    */
/*         and incorporated herein by reference.                         */
/*         As the copyright holder, John McCalpin retains the            */
/*         right to determine conformity with the Run Rules.             */
/*     3b. Results based on modified source code or on runs not in       */
/*         accordance with the STREAM Run Rules must be clearly          */
/*         labelled whenever they are published.  Examples of            */
/*         proper labelling include:                                     */
/*           "tuned STREAM benchmark results"                            */
/*           "based on a variant of the STREAM benchmark code"           */
/*         Other comparable, clear, and reasonable labelling is          */
/*         acceptable.                                                   */
/*     3c. Submission of results to the STREAM benchmark web site        */
/*         is encouraged, but not required.                              */
/*  4. Use of this program or creation of derived works based on this    */
/*     program constitutes acceptance of these licensing restrictions.   */
/*  5. Absolutely no warranty is expressed or implied.                   */
/*-----------------------------------------------------------------------*/
#define _BSD_SOURCE
#define K2_DEBUG_VERBOSE 1
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

#ifndef PAGE_SIZE
#define PAGE_SIZE SZ_4K
#endif

#define NCPUS 4

//#define TUNED /* xzl */
#define HAS_DMA 	1 /* xzl */
#define HAS_NUMA	1
//#define HAS_CMEM	1

#if defined(HAS_NUMA) && defined(HAS_CMEM)
#error "config error?"
#endif

//#define VERBOSE 	1
/*-----------------------------------------------------------------------
 * INSTRUCTIONS:
 *
 *	1) STREAM requires different amounts of memory to run on different
 *           systems, depending on both the system cache size(s) and the
 *           granularity of the system timer.
 *     You should adjust the value of 'STREAM_ARRAY_SIZE' (below)
 *           to meet *both* of the following criteria:
 *       (a) Each array must be at least 4 times the size of the
 *           available cache memory. I don't worry about the difference
 *           between 10^6 and 2^20, so in practice the minimum array size
 *           is about 3.8 times the cache size.
 *           Example 1: One Xeon E3 with 8 MB L3 cache
 *               STREAM_ARRAY_SIZE should be >= 4 million, giving
 *               an array size of 30.5 MB and a total memory requirement
 *               of 91.5 MB.
 *           Example 2: Two Xeon E5's with 20 MB L3 cache each (using OpenMP)
 *               STREAM_ARRAY_SIZE should be >= 20 million, giving
 *               an array size of 153 MB and a total memory requirement
 *               of 458 MB.
 *       (b) The size should be large enough so that the 'timing calibration'
 *           output by the program is at least 20 clock-ticks.
 *           Example: most versions of Windows have a 10 millisecond timer
 *               granularity.  20 "ticks" at 10 ms/tic is 200 milliseconds.
 *               If the chip is capable of 10 GB/s, it moves 2 GB in 200 msec.
 *               This means the each array must be at least 1 GB, or 128M elements.
 *
 *      Version 5.10 increases the default array size from 2 million
 *          elements to 10 million elements in response to the increasing
 *          size of L3 caches.  The new default size is large enough for caches
 *          up to 20 MB.
 *      Version 5.10 changes the loop index variables from "register int"
 *          to "ssize_t", which allows array indices >2^32 (4 billion)
 *          on properly configured 64-bit systems.  Additional compiler options
 *          (such as "-mcmodel=medium") may be required for large memory runs.
 *
 *      Array size can be set at compile time without modifying the source
 *          code for the (many) compilers that support preprocessor definitions
 *          on the compile line.  E.g.,
 *                gcc -O -DSTREAM_ARRAY_SIZE=100000000 stream.c -o stream.100M
 *          will override the default size of 10M with a new size of 100M elements
 *          per array.
 */
#ifndef STREAM_ARRAY_SIZE
//#   define STREAM_ARRAY_SIZE	10000000
//#   define STREAM_ARRAY_SIZE	(10 * 1024 * 1024)
#   define STREAM_ARRAY_SIZE	(65536 * 2)
//#   define STREAM_ARRAY_SIZE	(512 * 1024)
//#   define STREAM_ARRAY_SIZE	800000
//#	define STREAM_ARRAY_SIZE	(6*1024*1024/8/3)		/* for all-on msmc, total ~6MB */
#endif

/*  2) STREAM runs each kernel "NTIMES" times and reports the *best* result
 *         for any iteration after the first, therefore the minimum value
 *         for NTIMES is 2.
 *      There are no rules on maximum allowable values for NTIMES, but
 *         values larger than the default are unlikely to noticeably
 *         increase the reported performance.
 *      NTIMES can also be set on the compile line without changing the source
 *         code using, for example, "-DNTIMES=7".
 */
#ifdef NTIMES
#if NTIMES<=1
#   define NTIMES	10
#endif
#endif
#ifndef NTIMES
#   define NTIMES	10
#endif

/*  Users are allowed to modify the "OFFSET" variable, which *may* change the
 *         relative alignment of the arrays (though compilers may change the
 *         effective offset by making the arrays non-contiguous on some systems).
 *      Use of non-zero values for OFFSET can be especially helpful if the
 *         STREAM_ARRAY_SIZE is set to a value close to a large power of 2.
 *      OFFSET can also be set on the compile line without changing the source
 *         code using, for example, "-DOFFSET=56".
 */
#ifndef OFFSET
#   define OFFSET	0
#endif

/*
 *	3) Compile the code with optimization.  Many compilers generate
 *       unreasonably bad code before the optimizer tightens things up.
 *     If the results are unreasonably good, on the other hand, the
 *       optimizer might be too smart for me!
 *
 *     For a simple single-core version, try compiling with:
 *            cc -O stream.c -o stream
 *     This is known to work on many, many systems....
 *
 *     To use multiple cores, you need to tell the compiler to obey the OpenMP
 *       directives in the code.  This varies by compiler, but a common example is
 *            gcc -O -fopenmp stream.c -o stream_omp
 *       The environment variable OMP_NUM_THREADS allows runtime control of the
 *         number of threads/cores used when the resulting "stream_omp" program
 *         is executed.
 *
 *     To run with single-precision variables and arithmetic, simply add
 *         -DSTREAM_TYPE=float
 *     to the compile line.
 *     Note that this changes the minimum array sizes required --- see (1) above.
 *
 *     The preprocessor directive "TUNED" does not do much -- it simply causes the
 *       code to call separate functions to execute each kernel.  Trivial versions
 *       of these functions are provided, but they are *not* tuned -- they just
 *       provide predefined interfaces to be replaced with tuned code.
 *
 *
 *	4) Optional: Mail the results to mccalpin@cs.virginia.edu
 *	   Be sure to include info that will help me understand:
 *		a) the computer hardware configuration (e.g., processor model, memory type)
 *		b) the compiler name/version and compilation flags
 *      c) any run-time information (such as OMP_NUM_THREADS)
 *		d) all of the output from the test case.
 *
 * Thanks!
 *
 *-----------------------------------------------------------------------*/

# define HLINE "-------------------------------------------------------------\n"

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif

#ifndef STREAM_TYPE
#define STREAM_TYPE double
#endif

#if 0
static STREAM_TYPE	a[STREAM_ARRAY_SIZE+OFFSET],
			b[STREAM_ARRAY_SIZE+OFFSET],
			c[STREAM_ARRAY_SIZE+OFFSET];
#else
static STREAM_TYPE *a = NULL, *b = NULL, *c = NULL;  /* xzl */
#endif

static double	avgtime[4] = {0}, maxtime[4] = {0},
		mintime[4] = {FLT_MAX,FLT_MAX,FLT_MAX,FLT_MAX};

static char	*label[4] = {"Copy:      ", "Scale:     ",
    "Add:       ", "Triad:     "};

static double	bytes[4] = {
    2 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
    2 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
    3 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE,
    3 * sizeof(STREAM_TYPE) * STREAM_ARRAY_SIZE
    };

extern double mysecond();
extern void checkSTREAMresults();
#ifdef TUNED
extern void tuned_STREAM_Copy();
extern void tuned_STREAM_Scale(STREAM_TYPE scalar);
extern void tuned_STREAM_Add();
extern void tuned_STREAM_Triad(STREAM_TYPE scalar);
#endif
#ifdef _OPENMP
extern int omp_get_num_threads();
#endif

/* msmc buffer size */
//#define PAGE_ORDER 7 	/* 128 pages, 512KB */
#define PAGE_ORDER 8 	/* 256 pages, 1M */
//#define PAGE_ORDER 9 	/* 512 pages, 2M */   // too many pages we don't support
#define PAGE_COUNT (1<<PAGE_ORDER)

//#define DESC_ORDER 5  // 32 pgs
//#define DESC_ORDER 6  // 64 pgs
#define DESC_ORDER 7  // 128 pgs, 512K

#define NUM_DESCS (1 << (PAGE_ORDER - DESC_ORDER))
#define DESC_SIZE ((1<<DESC_ORDER) * PAGE_SIZE) /* single desc, in bytes */

/* use the unified debugfs file. cannot close the file */
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

static int kernel_driver_test(mig_region **region)
{
	int configfd;
	char * address = NULL;

	configfd = open("/sys/kernel/debug/migif1", O_RDWR);
	if(configfd < 0) {
		perror("open");
		return -1;
	}

	/* the actual size does not matter */
	address = mmap(NULL, 3 * PAGE_SIZE, PROT_READ|PROT_WRITE,
			MAP_SHARED, configfd, 0);
	if (address == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	/* now the driver should have d/s ready; directly use them */
	*region = (mig_region *)address;
	V("mmap'd region is %08x ndescs %d", (uint32_t)region, (*region)->ndescs);

	return configfd;
}

void get_buffers_ddr_malloc(void)
{
	a = malloc((STREAM_ARRAY_SIZE + OFFSET)* sizeof(STREAM_TYPE));
	b = malloc((STREAM_ARRAY_SIZE + OFFSET) * sizeof(STREAM_TYPE));
	c = malloc((STREAM_ARRAY_SIZE + OFFSET) * sizeof(STREAM_TYPE));

	assert(a && b && c);

	/* touch them so that they fault in */
	memset(a, -1, (STREAM_ARRAY_SIZE + OFFSET)* sizeof(STREAM_TYPE));
	memset(b, -2, (STREAM_ARRAY_SIZE + OFFSET)* sizeof(STREAM_TYPE));
	memset(c, -3, (STREAM_ARRAY_SIZE + OFFSET)* sizeof(STREAM_TYPE));

	printf("xzl: get ddr buffer through malloc() seems okay\n");

	return;
}

static STREAM_TYPE	_a[STREAM_ARRAY_SIZE+OFFSET],
			_b[STREAM_ARRAY_SIZE+OFFSET],
			_c[STREAM_ARRAY_SIZE+OFFSET];

int get_buffers_ddr(void)
{
	a = _a;
	b = _b;
	c = _c;

	printf("buffer region: dram (global) \n");
	return 0;
}

/* ----------------------------
 * various consumer functions
 * ---------------------------- */
/* vector add */
static int consumer_add(char *buf, int size)
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
}
/* --------------------------------- */

static int consumer_traid(char *buf, int size)
{
	STREAM_TYPE *aa, *bb, *cc;
	int j;
	STREAM_TYPE scalar = 3.0;
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
		aa[j] = bb[j] + scalar * cc[j];
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
/* vector product */
static int consumer_vecprod(char *buf, int size)
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
		aa[j] = bb[j] * cc[j];
}

/* --------------------------------- */
static int consumer_dist(char *buf, int size)
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
int
main()
    {
    int			quantum, checktick();
    int			BytesPerWord;
    int			k;
    ssize_t		j;
    STREAM_TYPE		scalar;
    double		t, times[4][NTIMES];

    /* ---------- mig setup ----------- */
	int configfd;
	STREAM_TYPE *p; 	/* msmc buffer */
	mig_region *region = NULL;

	get_buffers_ddr_malloc();	// touches it
//	get_buffers_ddr();

    /* --- SETUP --- determine precision and check timing --- */

    printf(HLINE);
    printf("STREAM version $Revision: 5.10 $\n");
    printf(HLINE);
    BytesPerWord = sizeof(STREAM_TYPE);
    printf("This system uses %d bytes per array element.\n",
	BytesPerWord);

    printf(HLINE);
#ifdef N
    printf("*****  WARNING: ******\n");
    printf("      It appears that you set the preprocessor variable N when compiling this code.\n");
    printf("      This version of the code uses the preprocesor variable STREAM_ARRAY_SIZE to control the array size\n");
    printf("      Reverting to default value of STREAM_ARRAY_SIZE=%llu\n",(unsigned long long) STREAM_ARRAY_SIZE);
    printf("*****  WARNING: ******\n");
#endif

    printf("Array size = %llu (elements), Offset = %d (elements)\n" , (unsigned long long) STREAM_ARRAY_SIZE, OFFSET);
    printf("Memory per array = %.1f MiB (= %.1f GiB).\n",
	BytesPerWord * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.0),
	BytesPerWord * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.0/1024.0));
    printf("Total memory required = %.1f MiB (= %.1f GiB).\n",
	(3.0 * BytesPerWord) * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024.),
	(3.0 * BytesPerWord) * ( (double) STREAM_ARRAY_SIZE / 1024.0/1024./1024.));
    printf("Each kernel will be executed %d times.\n", NTIMES);
    printf(" The *best* time for each kernel (excluding the first iteration)\n");
    printf(" will be used to compute the reported bandwidth.\n");

#ifdef _OPENMP
    printf(HLINE);
#pragma omp parallel
    {
#pragma omp master
	{
	    k = omp_get_num_threads();
	    printf ("Number of Threads requested = %i\n",k);
        }
    }
#endif

#ifdef _OPENMP
	k = 0;
#pragma omp parallel
#pragma omp atomic
		k++;
    printf ("Number of Threads counted = %i\n",k);
#endif

    /* Get initial value for system clock. */
#pragma omp parallel for
    for (j=0; j<STREAM_ARRAY_SIZE; j++) {
	    a[j] = 1.0;
	    b[j] = 2.0;
	    c[j] = 0.0;
	}

    printf(HLINE);

    if  ( (quantum = checktick()) >= 1)
	printf("Your clock granularity/precision appears to be "
	    "%d microseconds.\n", quantum);
    else {
	printf("Your clock granularity appears to be "
	    "less than one microsecond.\n");
	quantum = 1;
    }

    t = mysecond();
#pragma omp parallel for
    for (j = 0; j < STREAM_ARRAY_SIZE; j++)
		a[j] = 2.0E0 * a[j];
    t = 1.0E6 * (mysecond() - t);

    printf("Each test below will take on the order"
	" of %d microseconds.\n", (int) t  );
    printf("   (= %d clock ticks)\n", (int) (t/quantum) );
    printf("Increase the size of the arrays if this shows that\n");
    printf("you are not getting at least 20 clock ticks per test.\n");

    printf(HLINE);

    printf("WARNING -- The above is only a rough guideline.\n");
    printf("For best results, please be sure you know the\n");
    printf("precision of your system timer.\n");
    printf(HLINE);

    /*	--- MAIN LOOP --- repeat test cases NTIMES times --- */

    /* ---- mig setup ------ */
#if HAS_DMA
	configfd = kernel_driver_test(&region);

	p = numa_alloc_onnode(NUM_DESCS * DESC_SIZE, 1);
	assert(p);
	I("mem on node1. vaddr %08lx -- %08lx", (unsigned long)p,
			(unsigned long)p + NUM_DESCS * DESC_SIZE);
	/* touch all msmc words */
	memset(p, 0, DESC_SIZE * NUM_DESCS);

	/* makes life easier */
	assert(STREAM_ARRAY_SIZE * BytesPerWord %
			(NUM_DESCS * DESC_SIZE) == 0);
	assert(STREAM_ARRAY_SIZE * BytesPerWord /
			(NUM_DESCS * DESC_SIZE) >= 1);

	I("num_descs %d, each %d KB (%d pages %d STREAM elements)",
			NUM_DESCS, DESC_SIZE/1024,
			(1<<DESC_ORDER), DESC_SIZE/BytesPerWord);
#endif

    scalar = 3.0;
    for (k=0; k<NTIMES; k++)
	{
#ifndef HAS_DMA
	times[0][k] = mysecond();
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
	    c[j] = a[j];
	times[0][k] = mysecond() - times[0][k];
#else
#if 1
//#ifdef HAS_NUMA
		{
			/* test the consumption speed of msmc & ddr */
			STREAM_TYPE *ddr;
			ddr = numa_alloc_onnode(NUM_DESCS * DESC_SIZE, 0);
			memset(ddr, 0, DESC_SIZE * NUM_DESCS);

			I("ddr -- num_descs %d, each %d KB (%d pages %d elements)",
					NUM_DESCS, DESC_SIZE/1024,
					(1<<DESC_ORDER), DESC_SIZE/BytesPerWord);

			clearcache((char *)ddr, NUM_DESCS * DESC_SIZE);
			clearcache((char *)p, NUM_DESCS * DESC_SIZE);

#if 1
			k2_measure("start");
			consumer_traid((char *)p, DESC_SIZE);
			k2_measure("traid-msmc-consumed");
			consumer_traid((char *)ddr, DESC_SIZE);
			k2_measure("traid-ddr-consumed");
			k2_flush_measure_format();
#endif

			k2_measure("start");

			clearcache((char *)ddr, NUM_DESCS * DESC_SIZE);
			clearcache((char *)p, NUM_DESCS * DESC_SIZE);

			consumer_dist((char *)ddr, DESC_SIZE);
			k2_measure("dist-ddr-consumed");

			consumer_dist((char *)p, DESC_SIZE);
			k2_measure("dist-msmc-consumed");

			k2_flush_measure_format();

			/* txt indexer */
#if 0
			wc_init();
			wc_fill_text((char *)p, NUM_DESCS * DESC_SIZE);
			k2_measure("start");
			consumer_wc_smp((char *)p, NUM_DESCS * DESC_SIZE);
			k2_measure("msmc-txt-consumed");
			wc_cleanup();

			wc_init();
			wc_fill_text((char *)ddr, NUM_DESCS * DESC_SIZE);
			k2_measure("start");
			consumer_wc_smp((char *)ddr, NUM_DESCS * DESC_SIZE);
			k2_measure("ddr-txt-consumed");
			wc_cleanup();

			k2_flush_measure_format();
#endif

			numa_free(ddr, NUM_DESCS * DESC_SIZE);
		}
#endif

#ifdef HAS_NUMA	/* NUMA+memif. generic code using 2 buffers */
        assert(NUM_DESCS == 2);
		{
			unsigned long msmc_size = NUM_DESCS * DESC_SIZE;
        	/* ddr: 20x of msmc */
			unsigned long ddr_size = 10 * NUM_DESCS * DESC_SIZE;
			char *ddr, *msmc;
			struct pollfd ep0_poll;
        	int half = 0, k;

        	msmc = p;
        	ddr = numa_alloc_onnode(ddr_size, 0);
//        	ddr = mmap(0, ddr_size,
//        		       (PROT_READ|PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        	I("mem on node0. vaddr %08lx", (unsigned long)ddr);
        	assert(msmc && ddr);

        	memset(ddr, 0, ddr_size);
        	memset(msmc, 0, msmc_size);

        	poke_driver(configfd, MIG_INIT_REGION);

        	ep0_poll.fd = configfd;
        	ep0_poll.events = POLLIN;

        	k2_measure("init-dma-start");
        	memif_start_replication(configfd, region, ddr,
        			msmc + msmc_size / 2 * half, DESC_ORDER);

        	for (k = 0; k < 10; k++) {
        		int newhalf = 1 - half;

        		/* wait for the old half to be ready */
				if (poll(&ep0_poll, 1, 100) == 0)
					W("poll timeout");

				k2_measure("dma-wait-end");

				/* XXX dequeue & free desc? XXX */

				if (k > 1) {
					/* writeback 1/6 msmc XXX --- not 1/8 */
					memif_start_replication(configfd, region, msmc,
					        ddr, DESC_ORDER-2);
					if (poll(&ep0_poll, 1, 100) == 0)
						W("poll timeout");
					k2_measure("msmc-wb-end");
				}

				/* move to the new half */
				memif_start_replication(configfd, region,
						ddr + msmc_size / 2 * k,
						msmc + msmc_size / 2 * newhalf, DESC_ORDER);

				/* consume the old half */
				consumer_traid((char *)(msmc + msmc_size / 2 * half),
							msmc_size / 2);

				k2_measure("consume-msmc-end");

				half = newhalf;
        	}
        	k2_flush_measure_format();

        	/* --- ddr (note this may be too optimistic due to
        	 * cache effect. better measure before doing everything)
        	 * --- */
        	k2_measure("consume-ddr-start");
        	consumer_traid(ddr, msmc_size);
        	k2_measure("consume-ddr-end");
        	k2_flush_measure_format();
		}
#endif /* HAS_NUMA */
#if 0
        {  /* memif move: DDR->MSMC MSMC->DDR */
        	mig_desc *descs[NUM_DESCS];
        	struct pollfd ep0_poll;
        	int ii, wait = 0;
        	color_t clr;

        	j = 0;	/* global index in the array */

        	poke_driver(configfd, MIG_INIT_REGION);

        	ep0_poll.fd = configfd;
        	ep0_poll.events = POLLIN;

        	times[0][k] = mysecond();

        	for (j = 0; j < STREAM_ARRAY_SIZE;
        			j += NUM_DESCS * DESC_SIZE  / BytesPerWord) {

				for (ii = 0; ii < NUM_DESCS; ii++)
					descs[ii] = freelist_remove(region);

				/* ddr --> msmc */
				for (ii = 0; ii < NUM_DESCS; ii++) {
					descs[ii]->virt_base = virt_to_base(a + j + ii * DESC_SIZE / BytesPerWord);
					descs[ii]->virt_base_dest = virt_to_base(p + ii * DESC_SIZE / BytesPerWord);
					descs[ii]->order = DESC_ORDER;

					V("src %08x dst %08x",
							(unsigned int)(a + j + ii * DESC_SIZE / BytesPerWord),
							(unsigned int)(p + ii * DESC_SIZE / BytesPerWord));

					/* submit the request */
					clr = mig_enqueue(region, &region->qreq, descs[ii]);
					if (clr == COLOR_USR)
						poke_driver(configfd, MIG_MOVE_SINGLE_NOREMAP);
				}

				/* --- time to sleep --- */
				if (poll(&ep0_poll, 1, 100) == 0)
					W("poll timeout");
				else
					V("poll okay");

				ii = 0; wait = 0;
				while (1) {
					if ((descs[ii] = mig_dequeue(region, &(region->qcomp)))) {
						D("retrieved a completed desc %d/%d", ii, NUM_DESCS-1);
						/* XXX put desc back to freelist? */
						if (++ii == NUM_DESCS)
							break;
					} else
						wait ++;
				};

				/* msmc --> ddr */
				for (ii = 0; ii < NUM_DESCS; ii++) {
					descs[ii]->virt_base = virt_to_base(p + ii * DESC_SIZE / BytesPerWord);
					descs[ii]->virt_base_dest = virt_to_base(c + j + ii * DESC_SIZE / BytesPerWord);
					descs[ii]->order = DESC_ORDER;
					/* submit the request */
					clr = mig_enqueue(region, &region->qreq, descs[ii]);
					if (clr == COLOR_USR)
						poke_driver(configfd, MIG_MOVE_SINGLE_NOREMAP);
				}

				/* --- time to sleep --- */
				if (poll(&ep0_poll, 1, 100) == 0)
					W("poll timeout");
				else
					V("poll okay");

				ii = 0; wait = 0;
				while (1) {
					if ((descs[ii] = mig_dequeue(region, &(region->qcomp)))) {
						D("retrieved a completed desc %d/%d", ii, NUM_DESCS-1);
						/* XXX put desc back to freelist? */
						if (++ii == NUM_DESCS)
							break;
					} else
						wait ++;
				};

				for (ii = 0; ii < NUM_DESCS; ii++)
					freelist_add(region, descs[ii]);
        	}
        	times[0][k] = mysecond() - times[0][k];
        	I("%.2f MBs in %.2f ms. %.3f MB/sec",
        			2 * 1.0 * STREAM_ARRAY_SIZE * BytesPerWord / 1024 /1024,
        			times[0][k] * 1000,
        			2 * 1.0 * STREAM_ARRAY_SIZE * BytesPerWord / 1024 /1024 / times[0][k]);
        }
#endif

#if 0
        { 	/* only use DMA to move DDR-->DDR */
        	mig_desc *descs[NUM_DESCS];
        	struct pollfd ep0_poll;
        	int ii, wait = 0;
        	color_t clr;

        	j = 0;	/* global index in the array */

        	poke_driver(configfd, MIG_INIT_REGION);

        	ep0_poll.fd = configfd;
        	ep0_poll.events = POLLIN;

        	times[0][k] = mysecond();

        	for (j = 0; j < STREAM_ARRAY_SIZE;
        			j += NUM_DESCS * DESC_SIZE  / BytesPerWord) {

				for (ii = 0; ii < NUM_DESCS; ii++)
					descs[ii] = freelist_remove(region);

				/* ddr --> ddr */
				for (ii = 0; ii < NUM_DESCS; ii++) {
					descs[ii]->virt_base = virt_to_base(a + j + ii * DESC_SIZE / BytesPerWord);
					descs[ii]->virt_base_dest = virt_to_base(c + j + ii * DESC_SIZE / BytesPerWord);

					descs[ii]->order = DESC_ORDER;

					V("src %08x dst %08x",
							(unsigned int)(a + j + ii * DESC_SIZE / BytesPerWord),
							(unsigned int)(c + j + ii * DESC_SIZE / BytesPerWord));

					/* submit the request */
					clr = mig_enqueue(region, &region->qreq, descs[ii]);
					if (clr == COLOR_USR)
						poke_driver(configfd, MIG_MOVE_SINGLE_NOREMAP);
				}

				/* --- time to sleep --- */
				if (poll(&ep0_poll, 1, 100) == 0)
					W("poll timeout");
				else
					V("poll okay");

				ii = 0; wait = 0;
				while (1) {
					if ((descs[ii] = mig_dequeue(region, &(region->qcomp)))) {
						D("retrieved a completed desc %d/%d", ii, NUM_DESCS-1);
						/* XXX put desc back to freelist? */
						if (++ii == NUM_DESCS)
							break;
					} else
						wait ++;
				};

				for (ii = 0; ii < NUM_DESCS; ii++)
					freelist_add(region, descs[ii]);
        	}

        	/* xzl -- why? */
        	c[STREAM_ARRAY_SIZE-1] = a[STREAM_ARRAY_SIZE-1];

        	times[0][k] = mysecond() - times[0][k];
        	I("%.2f MBs in %.2f ms. %.3f MB/sec",
        			2 * 1.0 * STREAM_ARRAY_SIZE * BytesPerWord / 1024 /1024,
        			times[0][k] * 1000,
        			2 * 1.0 * STREAM_ARRAY_SIZE * BytesPerWord / 1024 /1024 / times[0][k]);
        }
#endif
#if 0
        { 	/* DMA + CPU, each move half of the buffer --- not much better */

			mig_desc *descs[NUM_DESCS];
			struct pollfd ep0_poll;
			int ii, wait = 0, jj = 0, cnt;
			color_t clr;
			int base;

			j = 0;	/* global index in the array */

			poke_driver(configfd, MIG_INIT_REGION);

			ep0_poll.fd = configfd;
			ep0_poll.events = POLLIN;

			times[0][k] = mysecond();

			for (j = 0; j < STREAM_ARRAY_SIZE;
					j += NUM_DESCS * DESC_SIZE  / BytesPerWord) {

				for (ii = 0; ii < NUM_DESCS/2; ii++)
					descs[ii] = freelist_remove(region);

				/* ddr --> ddr. half of them */
				for (ii = 0; ii < NUM_DESCS/2; ii++) {
					descs[ii]->virt_base = virt_to_base(a + j + ii * DESC_SIZE / BytesPerWord);
					descs[ii]->virt_base_dest = virt_to_base(c + j + ii * DESC_SIZE / BytesPerWord);

					descs[ii]->order = DESC_ORDER;

					V("src %08x dst %08x",
							(unsigned int)(a + j + ii * DESC_SIZE / BytesPerWord),
							(unsigned int)(c + j + ii * DESC_SIZE / BytesPerWord));

					/* submit the request */
					clr = mig_enqueue(region, &region->qreq, descs[ii]);
					if (clr == COLOR_USR)
						poke_driver(configfd, MIG_MOVE_SINGLE_NOREMAP);
				}

				base = j + NUM_DESCS / 2 * DESC_SIZE / BytesPerWord;
				cnt =  NUM_DESCS / 2 * DESC_SIZE / BytesPerWord;
				D("start copy %d -- %d", base, base+cnt);
				for (jj = 0; jj < cnt; jj++)
					c[base + jj] = a[base + jj];

				/* --- time to sleep --- */
				if (poll(&ep0_poll, 1, 100) == 0)
					W("poll timeout");
				else
					V("poll okay");

				ii = 0; wait = 0;
				while (1) {
					if ((descs[ii] = mig_dequeue(region, &(region->qcomp)))) {
						D("retrieved a completed desc %d/%d", ii, NUM_DESCS/2-1);
						/* XXX put desc back to freelist? */
						if (++ii == NUM_DESCS/2)
							break;
					} else
						wait ++;
				};

				for (ii = 0; ii < NUM_DESCS/2; ii++)
					freelist_add(region, descs[ii]);
			}

			/* xzl -- why? */
			c[STREAM_ARRAY_SIZE-1] = a[STREAM_ARRAY_SIZE-1];

			times[0][k] = mysecond() - times[0][k];
			I("%.2f MBs in %.2f ms. %.3f MB/sec",
					2 * 1.0 * STREAM_ARRAY_SIZE * BytesPerWord / 1024 /1024,
					times[0][k] * 1000,
					2 * 1.0 * STREAM_ARRAY_SIZE * BytesPerWord / 1024 /1024 / times[0][k]);
		}
#endif

#if 0
        {  /* DMA move: DDR->MSMC; CPU compute, then move MSMC->DDR */
        	mig_desc *descs[NUM_DESCS];
        	struct pollfd ep0_poll;
        	int ii, wait = 0;
        	color_t clr;
			int words_per_desc = DESC_SIZE / BytesPerWord;
			int start, end;

        	j = 0;	/* global index in the array */

        	poke_driver(configfd, MIG_INIT_REGION);

        	ep0_poll.fd = configfd;
        	ep0_poll.events = POLLIN;

        	times[0][k] = mysecond();

#if 0
        	/* both ddr -- this is fast probably because we touched
        	 * all ddr memory? */
        	k2_measure("*ddr-start");
			#pragma omp parallel for
			for (j = 0; j < words_per_desc * NUM_DESCS; j++)
				c[j] = scalar * a[j];
        	k2_measure("ddr-end");

        	/* ddr/msmc */
        	k2_measure("*msmc-start");
			#pragma omp parallel for
			for (j = 0; j < words_per_desc * NUM_DESCS; j++)
				c[j] = scalar * p[j];
        	k2_measure("msmc-end");
        	k2_flush_measure_format();
#endif

        	for (j = 0; j < STREAM_ARRAY_SIZE;
        			j += NUM_DESCS * DESC_SIZE  / BytesPerWord) {

        		k2_measure("*start");

				for (ii = 0; ii < NUM_DESCS; ii++)
					descs[ii] = freelist_remove(region);

				k2_measure("descs-ready");

				/* ddr --> msmc */
				for (ii = 0; ii < NUM_DESCS; ii++) {
					descs[ii]->virt_base = virt_to_base(a + j + ii * DESC_SIZE / BytesPerWord);
					descs[ii]->virt_base_dest = virt_to_base(p + ii * DESC_SIZE / BytesPerWord);
					descs[ii]->order = DESC_ORDER;

					V("src %08x dst %08x",
							(unsigned int)(a + j + ii * DESC_SIZE / BytesPerWord),
							(unsigned int)(p + ii * DESC_SIZE / BytesPerWord));

					/* submit the request */
					clr = mig_enqueue(region, &region->qreq, descs[ii]);
					k2_measure("enqueue");

					if (ii == NUM_DESCS - 1) {
//					if (ii == NUM_DESCS - 1 && clr == COLOR_USR) {
//					if (clr == COLOR_USR) {
						D("poke %d", ii);
						poke_driver(configfd, MIG_MOVE_SINGLE_NOREMAP);
						k2_measure("poke");
					}
				}

				k2_measure("submit-end");

				ii = 0; wait = 0; // ii: the desc index

				/* whenever one desc is ready, process it */
				while (1) {
					int kk;  // the word index

					if (poll(&ep0_poll, 1, 1000) == 0)
						W("poll timeout");
					else
						V("poll okay");

					if ((descs[ii] = mig_dequeue(region, &(region->qcomp)))) {

						k2_measure("got-one-desc");

						D("retrieved a completed desc %d/%d", ii, NUM_DESCS-1);

						start = j + words_per_desc * ii;
						end = j + words_per_desc * (1 + ii);

						#pragma omp parallel for
						for (kk = start; kk < end; kk++)
							c[kk] = scalar * p[kk]; /* c's index seems wrong */

						k2_measure("X-one-desc");
						/* XXX put desc back to freelist? */
						if (++ii == NUM_DESCS)
							break;
					} else
						wait ++;
				};

				k2_measure("process-end");

				for (ii = 0; ii < NUM_DESCS; ii++)
					freelist_add(region, descs[ii]);
        	}
        	times[0][k] = mysecond() - times[0][k];
        	k2_flush_measure_format();

        	I("%.2f MBs in %.2f ms. %.3f MB/sec",
        			2 * 1.0 * STREAM_ARRAY_SIZE * BytesPerWord / 1024 /1024,
        			times[0][k] * 1000,
        			2 * 1.0 * STREAM_ARRAY_SIZE * BytesPerWord / 1024 /1024 / times[0][k]);
        }
#endif

#ifdef HAS_CMEM
        /* Generic code.
         * Using cmem to allocate data; using userspace dma to move data.
         * This requires vanilla kernel and TI's cmemk. */
# include "cmem-xzl.h"
# include "edma3-xzl.h"
#define DMA_BUFSIZE_MB 	200		// size of each dma buffer.
        {  /* DMA move: DDR->MSMC; runtime style -- maintain outstanding
         	  buffers. XXX */
        	int ii, wait = 0;
        	int half = 0;

        	void *channel;
        	void *xfer;  // handle

			// ddr buffers
			unsigned long ddr_base_phys = 0;	// actually dma addr
			char *ddr_base;
			unsigned long ddr_size = SZ_1M * DMA_BUFSIZE_MB;
			// msmc buffers
			unsigned long msmc_size = K2H_MSMC_SIZE;
			char *msmc_base;
			unsigned long msmc_base_phys = 0;	// actually dma addr

			static unsigned long ddr_offset = 0;	// virt - dma. fine either virt or dma is larger
			static unsigned long msmc_offset = 0;	// virt - dma. fine either virt or dma is larger

			#define ddr_dma2virt(d)  ((char *)(d + ddr_offset))
			#define ddr_virt2dma(v)	 ((unsigned long)v - ddr_offset)

			#define msmc_dma2virt(d)  ((char *)(d + msmc_offset))
			#define msmc_virt2dma(v)	 ((unsigned long)v - msmc_offset)

			STREAM_TYPE *aa, *bb, *cc;
			int k;

			EDMA3_DRV_Handle g_hdma;

			cmem_init();
			g_hdma = edma3_init();

			channel = edma3_allocate_channel(g_hdma);

			msmc_base = cmem_alloc_msmc(msmc_size, &msmc_base_phys);
		    msmc_offset = (unsigned long)msmc_base - msmc_base_phys;

		    ddr_base = cmem_alloc_ddr(ddr_size, &ddr_base_phys);
		    ddr_offset = (unsigned long)ddr_base - ddr_base_phys;

		    memset(ddr_base, 0, ddr_size);
		    memset(msmc_base, 0, msmc_size);

			k2_measure("init-dma-start");
//        	edma3_start_xfer_once(g_hdma, msmc_base_phys,
//        			msmc_base_phys + msmc_size/2,
//        			msmc_size/2, 0, &xfer);
			edma3_start_xfer_once(g_hdma, ddr_base_phys,
					msmc_base_phys + msmc_size / 2 * half,
					msmc_size / 2, 0, &xfer);

			/* in this loop, each iteration moves and also consumes half of
			 * the msmc.
			 * in consuming, the buffer (which is a half of msmc) is sliced
			 * into three as aa, bb, and cc.
			 *
			 * rough numbers in us:
			 * consuming msmc (half) w/ traid  -- 800
			 * msmc wb -- 120
			 * dma wait -- 4
			 *
			 * consume ddr w/ traid (same size) -- 4400
			 */
		    for (k = 0; k < 10; k++) {
		    	int newhalf = 1 - half;

		    	/* wait for the old half to be ready */
				edma3_wait_xfer(xfer);
				k2_measure("dma-wait-end");

				if (k > 1) {
					/* write back 1/3 XXX we dont use the right location */
					edma3_start_xfer_once(g_hdma, msmc_base_phys,
							ddr_base_phys,
							msmc_size/6, 0, &xfer);
					edma3_wait_xfer(xfer);
					k2_measure("msmc-wb-end");
				}

		    	/* start a xfer to the new half */
//				k2_measure("prev-dma-end");
	//        	edma3_start_xfer_once(g_hdma, msmc_base_phys,
	//        			msmc_base_phys + msmc_size/2,
	//        			msmc_size/2, 0, &xfer);
				edma3_start_xfer_once(g_hdma, ddr_base_phys,
						msmc_base_phys + msmc_size / 2 * newhalf,
						msmc_size / 2, 0, &xfer);

//				k2_measure("start-dma-end");

				/* consume the old half */
#if 0 /* to del */
				aa = (STREAM_TYPE *)(msmc_base + msmc_size / 2 * half);
				bb = (STREAM_TYPE *)(msmc_base + msmc_size / 2 * half + msmc_size / 6);
				cc = (STREAM_TYPE *)(msmc_base + msmc_size / 2 * half + 2 * msmc_size / 6);

				#pragma omp parallel for
				for (j = 0; j < msmc_size/6/BytesPerWord; j++)
					aa[j] = bb[j] + scalar * cc[j];
#endif
//				consumer_traid(msmc_base + msmc_size / 2 * half, msmc_size / 2);
//				consumer_sort(msmc_base + msmc_size / 2 * half, msmc_size / 2);
//				consumer_vecprod(msmc_base + msmc_size / 2 * half, msmc_size / 2);
				consumer_add(msmc_base + msmc_size / 2 * half, msmc_size / 2);

				k2_measure("consume-msmc-end");

				half = newhalf;
		    }
		    k2_flush_measure_format();

        	/* --- ddr --- */
        	k2_measure("consume-ddr-start");
#if 0 /* to del */
        	/* consuming the data */
        	aa = (STREAM_TYPE *)ddr_base;
			bb = (STREAM_TYPE *)(ddr_base + msmc_size / 3);
			cc = (STREAM_TYPE *)(ddr_base + 2 * msmc_size / 3);

			#pragma omp parallel for
        	for (j = 0; j < msmc_size/3/BytesPerWord; j++)
        		aa[j] = bb[j] + scalar * cc[j];
#endif

//        	consumer_traid(ddr_base, msmc_size);
//        	consumer_sort(ddr_base, msmc_size);
//        	consumer_vecprod(ddr_base, msmc_size);
        	consumer_add(ddr_base, msmc_size);

        	k2_measure("consume-ddr-end");
done:
        	k2_flush_measure_format();

        	exit(1);
        }
#endif

#if 0
        /*  -- verifiy --- */
        for (j = 0; j < STREAM_ARRAY_SIZE; j++) {
        	if (j < 10)
        		I("-- %d. c: %08lx vs a: %08lx", j,
        		        (unsigned long)c[j], (unsigned long)a[j]);
        	if ((unsigned long)c[j] != (unsigned long)a[j]) {
        		E("mismatch at %08x/%08x. c: %08lx vs a: %08lx", j, STREAM_ARRAY_SIZE,
        				(unsigned long)c[j], (unsigned long)a[j]);
        		abort();
        	}
        }
#endif
        E("bye"); abort();
#endif
	times[1][k] = mysecond();

#ifdef HAS_DMA
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
	    b[j] = scalar*c[j];
#else
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
	    b[j] = scalar*c[j];
#endif
	times[1][k] = mysecond() - times[1][k];

	times[2][k] = mysecond();
#ifdef TUNED
        tuned_STREAM_Add();
#else
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
	    c[j] = a[j]+b[j];
#endif
	times[2][k] = mysecond() - times[2][k];

	times[3][k] = mysecond();
#ifdef TUNED
        tuned_STREAM_Triad(scalar);
#else
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
	    a[j] = b[j]+scalar*c[j];
#endif
	times[3][k] = mysecond() - times[3][k];
	}

    /*	--- SUMMARY --- */

    for (k=1; k<NTIMES; k++) /* note -- skip first iteration */
	{
	for (j=0; j<4; j++)
	    {
	    avgtime[j] = avgtime[j] + times[j][k];
	    mintime[j] = MIN(mintime[j], times[j][k]);
	    maxtime[j] = MAX(maxtime[j], times[j][k]);
	    }
	}

    printf("Function    Best Rate MB/s  Avg time     Min time     Max time\n");
    for (j=0; j<4; j++) {
		avgtime[j] = avgtime[j]/(double)(NTIMES-1);

		printf("%s%12.1f  %11.6f  %11.6f  %11.6f\n", label[j],
	       1.0E-06 * bytes[j]/mintime[j],
	       avgtime[j],
	       mintime[j],
	       maxtime[j]);
    }
    printf(HLINE);

    /* --- Check Results --- */
    checkSTREAMresults();
    printf(HLINE);

    return 0;
}

# define	M	20

int
checktick()
    {
    int		i, minDelta, Delta;
    double	t1, t2, timesfound[M];

/*  Collect a sequence of M unique time values from the system. */

    for (i = 0; i < M; i++) {
	t1 = mysecond();
	while( ((t2=mysecond()) - t1) < 1.0E-6 )
	    ;
	timesfound[i] = t1 = t2;
	}

/*
 * Determine the minimum difference between these M values.
 * This result will be our estimate (in microseconds) for the
 * clock granularity.
 */

    minDelta = 1000000;
    for (i = 1; i < M; i++) {
	Delta = (int)( 1.0E6 * (timesfound[i]-timesfound[i-1]));
	minDelta = MIN(minDelta, MAX(Delta,0));
	}

   return(minDelta);
    }



/* A gettimeofday routine to give access to the wall
   clock timer on most UNIX-like systems.  */

#include <sys/time.h>

double mysecond()
{
        struct timeval tp;
        struct timezone tzp;

        gettimeofday(&tp,&tzp);
        return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

#ifndef abs
#define abs(a) ((a) >= 0 ? (a) : -(a))
#endif
void checkSTREAMresults ()
{
	STREAM_TYPE aj,bj,cj,scalar;
	STREAM_TYPE aSumErr,bSumErr,cSumErr;
	STREAM_TYPE aAvgErr,bAvgErr,cAvgErr;
	double epsilon;
	ssize_t	j;
	int	k,ierr,err;

    /* reproduce initialization */
	aj = 1.0;
	bj = 2.0;
	cj = 0.0;
    /* a[] is modified during timing check */
	aj = 2.0E0 * aj;
    /* now execute timing loop */
	scalar = 3.0;
	for (k=0; k<NTIMES; k++)
        {
            cj = aj;
            bj = scalar*cj;
            cj = aj+bj;
            aj = bj+scalar*cj;
        }

    /* accumulate deltas between observed and expected results */
	aSumErr = 0.0;
	bSumErr = 0.0;
	cSumErr = 0.0;
	for (j=0; j<STREAM_ARRAY_SIZE; j++) {
		aSumErr += abs(a[j] - aj);
		bSumErr += abs(b[j] - bj);
		cSumErr += abs(c[j] - cj);
		// if (j == 417) printf("Index 417: c[j]: %f, cj: %f\n",c[j],cj);	// MCCALPIN
	}
	aAvgErr = aSumErr / (STREAM_TYPE) STREAM_ARRAY_SIZE;
	bAvgErr = bSumErr / (STREAM_TYPE) STREAM_ARRAY_SIZE;
	cAvgErr = cSumErr / (STREAM_TYPE) STREAM_ARRAY_SIZE;

	if (sizeof(STREAM_TYPE) == 4) {
		epsilon = 1.e-6;
	}
	else if (sizeof(STREAM_TYPE) == 8) {
		epsilon = 1.e-13;
	}
	else {
		printf("WEIRD: sizeof(STREAM_TYPE) = %u\n",sizeof(STREAM_TYPE));
		epsilon = 1.e-6;
	}

	err = 0;
	if (abs(aAvgErr/aj) > epsilon) {
		err++;
		printf ("Failed Validation on array a[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
		printf ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",aj,aAvgErr,abs(aAvgErr)/aj);
		ierr = 0;
		for (j=0; j<STREAM_ARRAY_SIZE; j++) {
			if (abs(a[j]/aj-1.0) > epsilon) {
				ierr++;
#ifdef VERBOSE
				if (ierr < 10) {
					printf("         array a: index: %ld, expected: %e, observed: %e, relative error: %e\n",
						j,aj,a[j],abs((aj-a[j])/aAvgErr));
				}
#endif
			}
		}
		printf("     For array a[], %d errors were found.\n",ierr);
	}
	if (abs(bAvgErr/bj) > epsilon) {
		err++;
		printf ("Failed Validation on array b[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
		printf ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",bj,bAvgErr,abs(bAvgErr)/bj);
		printf ("     AvgRelAbsErr > Epsilon (%e)\n",epsilon);
		ierr = 0;
		for (j=0; j<STREAM_ARRAY_SIZE; j++) {
			if (abs(b[j]/bj-1.0) > epsilon) {
				ierr++;
#ifdef VERBOSE
				if (ierr < 10) {
					printf("         array b: index: %ld, expected: %e, observed: %e, relative error: %e\n",
						j,bj,b[j],abs((bj-b[j])/bAvgErr));
				}
#endif
			}
		}
		printf("     For array b[], %d errors were found.\n",ierr);
	}
	if (abs(cAvgErr/cj) > epsilon) {
		err++;
		printf ("Failed Validation on array c[], AvgRelAbsErr > epsilon (%e)\n",epsilon);
		printf ("     Expected Value: %e, AvgAbsErr: %e, AvgRelAbsErr: %e\n",cj,cAvgErr,abs(cAvgErr)/cj);
		printf ("     AvgRelAbsErr > Epsilon (%e)\n",epsilon);
		ierr = 0;
		for (j=0; j<STREAM_ARRAY_SIZE; j++) {
			if (abs(c[j]/cj-1.0) > epsilon) {
				ierr++;
#ifdef VERBOSE
				if (ierr < 10) {
					printf("         array c: index: %ld, expected: %e, observed: %e, relative error: %e\n",
						j,cj,c[j],abs((cj-c[j])/cAvgErr));
				}
#endif
			}
		}
		printf("     For array c[], %d errors were found.\n",ierr);
	}
	if (err == 0) {
		printf ("Solution Validates: avg error less than %e on all three arrays\n",epsilon);
	}
#ifdef VERBOSE
	printf ("Results Validation Verbose Results: \n");
	printf ("    Expected a(1), b(1), c(1): %f %f %f \n",aj,bj,cj);
	printf ("    Observed a(1), b(1), c(1): %f %f %f \n",a[1],b[1],c[1]);
	printf ("    Rel Errors on a, b, c:     %e %e %e \n",abs(aAvgErr/aj),abs(bAvgErr/bj),abs(cAvgErr/cj));
#endif
}

#ifdef TUNED
/* stubs for "tuned" versions of the kernels */
void tuned_STREAM_Copy()
{
	ssize_t j;
#pragma omp parallel for
        for (j=0; j<STREAM_ARRAY_SIZE; j++)
            c[j] = a[j];
}

void tuned_STREAM_Scale(STREAM_TYPE scalar)
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
	    b[j] = scalar*c[j];
}

void tuned_STREAM_Add()
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
	    c[j] = a[j]+b[j];
}

void tuned_STREAM_Triad(STREAM_TYPE scalar)
{
	ssize_t j;
#pragma omp parallel for
	for (j=0; j<STREAM_ARRAY_SIZE; j++)
	    a[j] = b[j]+scalar*c[j];
}
/* end of stubs for the "tuned" versions of the kernels */
#endif
