/*
 *	Use NUMA to speed up matrix multiplication.
 *
 *	Felix Xiaozhu Lin and Xu Liu, 2015.
 *
 *	For project memif.
 *
 *	Known issues: after memory across NUMA nodes, the performance is not
 *	as expected. Could there be some kernel bugs?
 */

#define _GNU_SOURCE		// for MAP_ANONYMOUS
#define _ISOC11_SOURCE	// for aligned_alloc

#include<stdlib.h>
#include<stdio.h>
#include<time.h>
#include<numaif.h>
#include<numa.h>
#include<assert.h>

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
#include <malloc.h>

#include "mig.h"
#include "log-xzl.h"
#include "measure.h"
#include "misc-xzl.h"

/*
 * dimension -- mat size
 * 256	256k
 * 512	1024k
 * 1024 4096k
 */
#define ROWS  512      // Number of rows in each matrix
#define COLUMNS  512   // Number of columns in each matrix
#define MAT_SIZE (4 * ROWS * COLUMNS)

#ifndef PAGE_SIZE
#define PAGE_SIZE SZ_4K
#endif

static float **matrix_a ;    // Left matrix operand
static float **matrix_b ;    // Right matrix operand
static float **matrix_r ;    // Matrix result

FILE *result_file ;

#define MSMC_BUFFER_SIZE	(4 * SZ_1M)
//#define DDR_BUFFER_SIZE		(128 * SZ_1M)
#define DDR_BUFFER_SIZE		(8 * SZ_1M)

static char *the_msmc_buffer = NULL;
static char *the_ddr_buffer = NULL;

/* x: the index of matrix entry */
#define msmc_buffer(x) 	((float *)(the_msmc_buffer + x * MAT_SIZE))
#define ddr_buffer(x) 	((float *)(the_ddr_buffer + x * MAT_SIZE))

/* we use contig buffer for a mat...
 * accessor: m points to the buffer beginning
 * */
#define MAT(m, x, y) ( *( (float *)m + x * COLUMNS + y) )

/* allocate the whole msmc buffer in one mmap() and chop it into
 * slices, each of which contains one matrix.
 */
static void init_msmc_buffers(void)
{
	int i;

	the_msmc_buffer = numa_alloc_onnode(MSMC_BUFFER_SIZE, 1);

//	the_msmc_buffer = mmap(0, DDR_BUFFER_SIZE,
//				(PROT_READ|PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	assert(the_msmc_buffer);

#if 0
	/* touch the buffer so it faults in */
	numa_set_preferred(1);
	for (i = 0 ; i < MSMC_BUFFER_SIZE/4; i++) {
		*((float *)the_msmc_buffer + i) = (float) rand() / RAND_MAX;
//		*((float *)the_msmc_buffer + i) = 0;
	}
#endif

	/* this seem to matter */
	clearcache((unsigned char *)the_msmc_buffer, MSMC_BUFFER_SIZE);

//	memset(the_msmc_buffer, 0, MSMC_BUFFER_SIZE);
}

static void init_ddr_matrices(void)
{
	int i;

//	the_ddr_buffer = mmap(0, DDR_BUFFER_SIZE,
//			(PROT_READ|PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

//	the_ddr_buffer = numa_alloc_onnode(MSMC_BUFFER_SIZE, 0);

	/* using posix API... */
	the_ddr_buffer = memalign(4096, MSMC_BUFFER_SIZE);

//	the_ddr_buffer = numa_alloc_local(MSMC_BUFFER_SIZE);

	assert(the_ddr_buffer);

	numa_set_preferred(0);

#if 0
	/* populate all matrices with random values (including matrix r)*/
	for (i = 0 ; i < DDR_BUFFER_SIZE/4; i++) {
		*((float *)the_ddr_buffer + i) = (float) rand() / RAND_MAX;
//		*((float *)the_ddr_buffer + i) = 0;
	}
#endif

	/* this seem to matter */
	clearcache((unsigned char *)the_ddr_buffer, MSMC_BUFFER_SIZE);

//	memset(the_ddr_buffer, 0, DDR_BUFFER_SIZE);
}

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
	V("mmap'd region is %08x ndescs %d", (uint32_t)region, region->ndescs);

	return configfd;
}

void initialize_matrices()
{
	int i,j;

	matrix_a = (float **) malloc(ROWS*sizeof(float*));
	matrix_b = (float **) malloc(ROWS*sizeof(float*));
	matrix_r = (float **) malloc(ROWS*sizeof(float*));

	/* 512x512 matrices
	 *
	 * a 		b 			r		secs
	 * posix_memalign	posix_memalign	posix_memalign	6
	 * posix_memalign	numaalloc(1)	posix_memalign	2
	 * posix_memalign	numaalloc(0)	posix_memalign	2
	 * posix_memalign	numa_alloc_local()	posix_memalign	2
	 * memalign		numaalloc(0)	memalign		2
	 * memalign		numaalloc(1)	memalign		2
	 * memalign		memalign		memalign		6
	 *
	 * numaalloc(0)	numaalloc(1)	numaalloc(0)	6
	 * numaalloc(1)	numaalloc(1)	numaalloc(0)	6
	 * mmap			numaalloc(1)	mmap			6
	 *
	 * NOTE: aligned_alloc undefined (in libc < 2.16?)
	 */
	numa_set_strict(1);

	for ( i = 0 ; i < ROWS ; i++) {
//		matrix_a[i] = (float*)memalign(4096, COLUMNS*sizeof(float));
//		posix_memalign(&matrix_a[i], 4096, COLUMNS*sizeof(float));
		matrix_a[i] = (float*)mmap(0, COLUMNS*sizeof(float), (PROT_READ|PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
//		matrix_a[i] = (float*)numa_alloc_onnode(COLUMNS*sizeof(float), 0);
		assert(matrix_a[i]);

//		  matrix_b[i] = (float*)memalign(4096, COLUMNS*sizeof(float));
//		posix_memalign(&matrix_b[i], 4096, COLUMNS*sizeof(float));
		matrix_b[i] = (float*)numa_alloc_onnode(COLUMNS*sizeof(float), 1);
//		matrix_b[i] = (float*)numa_alloc_local(COLUMNS*sizeof(float));
//		matrix_b[i] = (float*)mmap(0, COLUMNS*sizeof(float), (PROT_READ|PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		assert(matrix_b[i]);

//		  matrix_r[i] = (float*)memalign(4096, COLUMNS*sizeof(float));
//		posix_memalign(&matrix_r[i], 4096, COLUMNS*sizeof(float));
		matrix_r[i] = (float*)mmap(0, COLUMNS*sizeof(float), (PROT_READ|PROT_WRITE), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
//		matrix_r[i] = (float*)numa_alloc_onnode(COLUMNS*sizeof(float), 0);
		assert(matrix_r[i]);
	}

	k2_measure("mem-alloc-done");

	for ( i = 0 ; i < ROWS ; i++) {
		for (j = 0 ; j < COLUMNS ; j++) {
			matrix_a[i][j] = (float) rand() / RAND_MAX ;
			matrix_b[i][j] = (float) rand() / RAND_MAX ;
			matrix_r[i][j] = 0.0 ;
		}
	}

//	E("allocation+init done, press any key"); getchar();

	k2_measure("mem-init-done");

#if 0
	const int target = 1;
	int node, ret;
	int *nodes, *status;
	assert(!ret);

	nodes = malloc(ROWS * sizeof(int));
	status = malloc(ROWS * sizeof(int));
	assert(nodes && status);

	{

		int configfd;
		mig_region *region = NULL;
		mig_desc *desc;
		struct pollfd ep0_poll;

		configfd = kernel_driver_test(&region);
		poke_driver(configfd, MIG_INIT_REGION);

		desc = freelist_remove(region);
		assert(desc);

		ep0_poll.fd = configfd;
		ep0_poll.events = POLLIN;

		for (i = 0; i < ROWS; i++) {
			desc->virt_base = virt_to_base(matrix_b[i]);
			desc->order = 0;
			desc->node = 1;
			mig_enqueue(region, &region->qreq, desc);
			poke_driver(configfd, MIG_MOVE_SINGLE_MIG);

			if (poll(&ep0_poll, 1, 100) == 0) {
				E("poll timeout");
				exit(1);
			} else
				V("poll okay");

			desc = mig_dequeue(region, &(region->qcomp));
			V("%d: mig one page", i);
		}
		E("mig done, press any key");
		//	  getchar();
		E("continue to exec...");
	}
#endif

#if 0
	ret = move_pages(0, ROWS, (void **)matrix_b, nodes, status, 0);
	if (!ret) {
		perror("move_pages");
		exit(1);
	}
#endif

#if 0
	for ( i = 0 ; i < ROWS ; i++) {
		//	  printf("%p\n", matrix_b[i]);
		ret = move_pages(0, 1, (void **)&(matrix_b[i]), &target, &node, 0);
		printf("%d", node);
		if (ret != 0 || node != 1)
			printf("error in page move\n");
	}
#endif

	k2_measure("mig-done");

}

void print_result()
{
	int i,j;
	for ( i = 0 ; i < ROWS ; i++) {
		for ( j = 0 ; j < COLUMNS ; j++) {
			fprintf(result_file, "%6.4f ", matrix_r[i][j]) ;
		}
		fprintf(result_file, "\n") ;
	}
}

void multiply_matrices()
{
	int i,j,k;
	for (i = 0 ; i < ROWS ; i++) {
		for (j = 0 ; j < COLUMNS ; j++) {
			float sum = 0.0 ;
			for ( k = 0 ; k < COLUMNS ; k++) {
				sum = sum + matrix_a[i][k] * matrix_b[k][j] ;
			}
			matrix_r[i][j] = sum;
		}
	}
}

/* xzl */
static void multiply(float *a, float *b, float *r)
{
	int i,j,k;

	#pragma omp parallel for
	for (i = 0 ; i < ROWS ; i++) {
	#pragma omp parallel for
		for (j = 0 ; j < COLUMNS ; j++) {
			float sum = 0.0 ;
			for ( k = 0 ; k < COLUMNS ; k++) {
				sum = sum + MAT(a, i, k) * MAT(b, k, j) ;
			}
			MAT(r, i, j) = sum;
		}
	}
}

static int cmpfunc (const void * a, const void * b)
{
   return ( *(float*)a - *(float*)b );
}

static void sort(float *a, int count)
{
	qsort(a, count, sizeof(float), cmpfunc);
}

static void traid(float *a, float *b, float *r, int count)
{
	int i = 0;
	#pragma omp parallel for
	for (i = 0; i < count; i++) {
		r[i] = a[i] + 2 * b[i];
	}
}

int main()
{
	malloc(1111);
	if ((result_file = fopen("classic.txt", "w")) == NULL) {
		fprintf(stderr, "Couldn't open result file\n") ;
		perror("classic") ;
		return( EXIT_FAILURE ) ;
	}

	fprintf(result_file, "Classic matrix multiplication\n") ;

#if 0
	initialize_matrices();
	k2_measure("start");
	multiply_matrices();
	k2_measure("multiply1-done");
#endif

	init_msmc_buffers();
	init_ddr_matrices();

	/* purge the cache.. */
//	memset(the_ddr_buffer, 0xdeadbeef, DDR_BUFFER_SIZE);

	k2_measure("init-msmc/ddr/-done");
//	E("allocation+init done, press any key"); getchar();E("cont...");

#if 0	/* sort */
	sort(msmc_buffer(0), ROWS * COLUMNS);
	sort(msmc_buffer(1), ROWS * COLUMNS);
	sort(msmc_buffer(2), ROWS * COLUMNS);
	k2_measure("sort-msmc-done");

	sort(ddr_buffer(1), ROWS * COLUMNS);
	sort(ddr_buffer(2), ROWS * COLUMNS);
	sort(ddr_buffer(3), ROWS * COLUMNS);
	k2_measure("sort-ddr-done");
#endif

#if 0	/* traid */
	traid(msmc_buffer(0), msmc_buffer(1), msmc_buffer(2), ROWS * COLUMNS);
//	traid(msmc_buffer(3), msmc_buffer(4), msmc_buffer(5), ROWS * COLUMNS);
//	traid(msmc_buffer(6), msmc_buffer(7), msmc_buffer(8), ROWS * COLUMNS);
	k2_measure("traid-msmc-done");
#endif

#if 1
	/* migration */
	{
		int nodemask = 1;
		int ret;

//		numa_set_preferred(1);
		ret = mbind(the_ddr_buffer, MSMC_BUFFER_SIZE, MPOL_BIND, &nodemask, 2,
				MPOL_MF_MOVE);
		if (ret)
			perror("mbind");
//		getchar();
	}

	k2_measure("traid-ddr-start");
	traid(ddr_buffer(0), ddr_buffer(1), ddr_buffer(2), ROWS * COLUMNS);
//	traid(ddr_buffer(3), ddr_buffer(4), ddr_buffer(5), ROWS * COLUMNS);
//	traid(ddr_buffer(6), ddr_buffer(7), ddr_buffer(8), ROWS * COLUMNS);
	k2_measure("traid-ddr-done");
#endif

#if 0	/* matmpy */
	multiply(ddr_buffer(0), ddr_buffer(1), ddr_buffer(2));
	k2_measure("multiply-ddr-done");

	multiply(msmc_buffer(0), msmc_buffer(1), msmc_buffer(2));
	k2_measure("multiply-msmc-done");

	multiply(msmc_buffer(0), msmc_buffer(2), msmc_buffer(1));
	k2_measure("multiply-msmc-done");

	multiply(ddr_buffer(3), ddr_buffer(4), ddr_buffer(5));
	k2_measure("multiply-ddr-done");
#endif

	//        multiply_matrices();
	//        k2_measure("multiply2-done");
	//        print_elapsed_time() ;
	//	fprintf(result_file,"matrix_a: begin=%llx, end=%llx\n",(long long)&matrix_a[0][0],(long long)&matrix_a[ROWS-1][COLUMNS-1]);
	//	fprintf(result_file,"matrix_b: begin=%llx, end=%llx\n",(long long)&matrix_b[0][0],(long long)&matrix_b[ROWS-1][COLUMNS-1]);
	//	fprintf(result_file,"matrix_r: begin=%llx, end=%llx\n",(long long)&matrix_r[0][0],(long long)&matrix_r[ROWS-1][COLUMNS-1]);

	k2_flush_measure_format();

	fclose(result_file) ;

	return( 0 ) ;
}
