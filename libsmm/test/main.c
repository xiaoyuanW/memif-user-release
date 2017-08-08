/*
 * Test for TI's userspace edma3 driver.
 *
 * The contig phys memory is allocated with TI CMEM usr/kernel support.
 * Thus, cmem.ko must be loaded and it should manage carveout memory chunks.
 *
 * Felix Xiaozhu Lin <linxz02@gmail.com>, 2015.
 *
 * For project memif.
 */

# include <stdio.h>
# include <unistd.h>
# include <math.h>
# include <float.h>
# include <limits.h>
# include <sys/types.h>
# include <sys/time.h>
# include <assert.h>
# include <signal.h>
#include <stdlib.h>

#include "../cmem-xzl.h"
#include "../edma3-xzl.h"
#include "../log.h"

//#define K2_NO_MEASUREMENT 	1
#include "../measure.h"

#define K2H_MSMC_PHYS	0x0c000000
/* a safe place in ddr serving as playground */
#define K2H_DDR_FREE_PHYS	(0x80000000 + SZ_1G)

static void *msmc, *ddr;
static unsigned long msmc_phys = 0, ddr_phys = 0;
//static int bufsize = 32 * SZ_1K;
//static int bufsize = 64 * SZ_1K;
//static unsigned long bufsize = 2 * SZ_1M;
static unsigned long bufsize = 3200 * SZ_1K;
//static int bufsize = 256 * SZ_1K;
//static int bufsize = 2560 * 1000;

static int cmem_alloc_test(void)
{
	int ret;
	int nblocks;
	CMEM_BlockAttrs blockattr;
	CMEM_AllocParams params;
	int i;

	ret = CMEM_init();
	assert(ret == 0);

	/* check cmem blocks */
	ret = CMEM_getNumBlocks(&nblocks);
	assert(ret == 0);
	printf("total cmem blocks: %d\n", nblocks);

	for (i = 0; i < nblocks; i++) {
		ret = CMEM_getBlockAttrs(i, &blockattr);
		assert (ret == 0);
		printf("block %d: %llx @ %lx\n", i, blockattr.size, blockattr.phys_base);
	}

	/* get msmc buffer (assuming blk 1) */
	params.type = CMEM_HEAP;
	params.flags = CMEM_CACHED;
	params.alignment = 64; // ?
	msmc = CMEM_alloc2(1, bufsize, &params);
	assert(msmc);

	msmc_phys = CMEM_getPhys(msmc);
	printf("msmc allocation okay. phys %lx\n", msmc_phys);

	/* get ddr buffer (assuming blk 0) */
	params.type = CMEM_POOL;
	params.flags = CMEM_CACHED;
	params.alignment = 64; // ?
	ddr = CMEM_alloc2(0, bufsize, &params);
	assert(ddr);
	ddr_phys = (CMEM_getPhys(ddr)) + 0x80000000;

	/* --- msmc --  */
	//	params.type = CMEM_HEAP;
	//	params.flags = CMEM_NONCACHED;
	//	params.alignment = 64; // ?
	//	ddr = CMEM_alloc2(1, bufsize, &params);
	//	assert(ddr);
	//	ddr_phys = CMEM_getPhys(ddr);

	printf("ddr allocation okay. phys %lx\n", ddr_phys);
}

// directly use my own cmem wrapper
static int cmem_alloc_test2(void)
{
	cmem_init();

	ddr = cmem_alloc_ddr(bufsize, &ddr_phys);
	msmc = cmem_alloc_msmc(bufsize, &msmc_phys);
}

void my_handler(int s){
	printf("Caught signal %d, clean up...\n",s);
	edma3_fini();
	   exit(1);
}

int main(int argc, char **argv)
{
	EDMA3_DRV_Handle h;
	void *xfer, *xfer2;
	int ret;
	unsigned long *p, *p2;
	int i;

	signal (SIGINT,my_handler);

	printf("======== test for edma3 userspace driver ========\n");

	h = edma3_init();

	//	cmem_alloc_test();
	cmem_alloc_test2();

	printf("cmem allocation is done. cat /proc/pid/maps to check...(press any key)\n");
//	getchar();

	/* fill the buffers */
	I("filled src buf. size = %d KB\n", bufsize/1024);
	for (p = (unsigned long *)msmc, p2 = (unsigned long *)ddr, i = 0;
	        i < bufsize / 4; i++) {
		p[i] = i;
		p2[i] = 0xdeadbeef;
	}

#if 0
	printf("performing cache maint...\n");
	ret = CMEM_cacheWb(msmc, bufsize);
	assert(!ret);
	ret = CMEM_cacheWbInv(ddr, bufsize);
	assert(!ret);
#else

	printf("skip cache maint...\n");
#endif

	W("---------- edma3 single xfer test --------------");
	emif_perfcnt_init();
	emif_perfcnt_start();

	k2_measure("xfer-start");
	edma3_start_xfer_once(h, msmc_phys, ddr_phys, bufsize, 0, &xfer);
	k2_measure("xfer-startokay");
	edma3_wait_xfer(xfer);
	k2_measure("**xfer-end");
	k2_flush_measure_format();

	emif_perfcnt_end();

	/* check the ddr buffer */
	for (p = (unsigned long *)ddr, i = 0; i < bufsize / 4; i++) {

		//		printf("%08x  ", p[i]);
		//		if (i % 8 == 7)
		//			printf("\n");

		if (p[i] != i) {
			printf("check failed. should be %08x, actual %08x\n", i, p[i]);
			for (int j = i; j < i + 128; j ++) {
				printf("%08x  ", p[j]);
				if (j % 8 == 7)
					printf("\n");
			}

			assert(0);
		}
	}
	W("check: all %d KB okay.\n", bufsize/1024);

	W("---------- edma3 2x single xfer test --------------");
	I("filled src buf. size = %d KB\n", bufsize/1024);
	for (p = (unsigned long *)msmc, p2 = (unsigned long *)ddr, i = 0;
	        i < bufsize / 4; i++) {
		p[i] = i;
		p2[i] = 0xdeadbeef;
	}
	edma3_start_xfer_once(h, msmc_phys, ddr_phys, bufsize/2, 0, &xfer);
	edma3_start_xfer_once(h, msmc_phys+bufsize/2, ddr_phys+bufsize/2,
								bufsize/2, 0, &xfer2);
	edma3_wait_xfer(xfer);
	edma3_wait_xfer(xfer2);

	/* check the ddr buffer */
	for (p = (unsigned long *)ddr, i = 0; i < bufsize / 4; i++) {
		if (p[i] != i) {
			printf("check failed. should be %08x, actual %08x\n", i, p[i]);
			for (int j = i; j < i + 128; j ++) {
				printf("%08x  ", p[j]);
				if (j % 8 == 7)
					printf("\n");
			}

			assert(0);
		}
	}
	W("check: all %d KB okay.\n", bufsize/1024);


	W("---------- edma3 link test --------------");
	I("filled src buf. size = %d KB\n", bufsize/1024);
	for (p = (unsigned long *)msmc, p2 = (unsigned long *)ddr, i = 0;
	        i < bufsize / 4; i++) {
		p[i] = i;
		p2[i] = 0xdeadbeef;
	}

	edma3_start_xfer_multiple(h, msmc_phys, ddr_phys, bufsize,
			10, // times for repeating the xfer
			&xfer);


	for (p = (unsigned long *)ddr, i = 0; i < bufsize / 4; i++) {

		//		printf("%08x  ", p[i]);
		//		if (i % 8 == 7)
		//			printf("\n");

		if (p[i] != i) {
			printf("check failed. should be %08x, actual %08x\n", i, p[i]);
			for (int j = i; j < i + 128; j ++) {
				printf("%08x  ", p[j]);
				if (j % 8 == 7)
					printf("\n");
			}
			assert(0);
		}
	}
	W("check: all %d KB okay.\n", bufsize/1024);

#if 0
	W("---------- edma3 edma3_start_xfer_once1() test --------------");
	I("filled src buf. size = %d KB", bufsize/1024);
	for (p = (unsigned long *)msmc, p2 = (unsigned long *)ddr, i = 0;
	        i < bufsize / 4; i++) {
		p[i] = i;
		p2[i] = 0xdeadbeef;
	}
	W("starting 8 xfers, each %lu KB", bufsize/8/1024);
	void *chandle;
	chandle = edma3_allocate_channel(h);
	for (i = 0; i < 8; i++) {
		edma3_start_xfer_once1(chandle,
				msmc_phys + i * bufsize/8,
				ddr_phys + i * bufsize/8,
				bufsize/8, &xfer);
		edma3_wait_xfer1(xfer);
	}
	edma3_free_channel(chandle);
	/* check the ddr buffer */
	for (p = (unsigned long *)ddr, i = 0; i < bufsize / 4; i++) {
		if (p[i] != i) {
			printf("check failed. should be %08x, actual %08x\n", i, p[i]);
			for (int j = i; j < i + 128; j ++) {
				printf("%08x  ", p[j]);
				if (j % 8 == 7)
					printf("\n");
			}
			assert(0);
		}
	}
	W("check: all %d KB okay.", bufsize/1024);
#endif

	W("---------- edma3 self chaining test --------------");
	I("filled src buf. size = %d KB", bufsize/1024);
	for (p = (unsigned long *)msmc, p2 = (unsigned long *)ddr, i = 0;
	        i < bufsize / 4; i++) {
		p[i] = i;
		p2[i] = 0xdeadbeef;
	}

	edma3_start_xfer_multiple_selfchaining(h, msmc_phys, ddr_phys, bufsize,
			16, // times for repeating the xfer
			&xfer);

//	getchar();

	for (p = (unsigned long *)ddr, i = 0; i < bufsize / 4; i++) {
//		printf("%08x  ", p[i]);
//		if (i % 8 == 7)
//			printf("\n");
		if (p[i] != i) {
			printf("check failed. should be %08x, actual %08x (%.2f)\n",
					i, p[i], 1.0 * i / (bufsize/4));
			for (int j = i; j < i + 128; j ++) {
				printf("%08x  ", p[j]);
				if (j % 8 == 7)
					printf("\n");
			}
			assert(0);
		}
	}
	W("check: all %d KB okay.", bufsize/1024);

	W("---------- test gather xfers --------------");
	I("filled src buf. size = %d KB", bufsize/1024);
	for (p = (unsigned long *)msmc, p2 = (unsigned long *)ddr, i = 0;
	        i < bufsize / 4; i++) {
		p[i] = i;
		p2[i] = 0xdeadbeef;
	}

	int cnt = 400; 	// how many xfers? XXX cannot go beyond 447?
	assert(bufsize % cnt == 0);
	void *pp = edma3_allocate_channel_linked(h, cnt);
	assert(pp);

	unsigned long * srcs = malloc(sizeof(unsigned long) * cnt);
	unsigned long * sizes = malloc(sizeof(unsigned long) * cnt);
	unsigned long voffset = (unsigned long)msmc - msmc_phys;
	assert(srcs && sizes);

	for (int j = 0; j < cnt; j++) {
		srcs[j] = (unsigned long)msmc + j * bufsize/cnt;
		sizes[j] = bufsize/cnt;
	}

	k2_measure("gather-start");
	xfer = edma3_start_gather(pp,
			srcs, sizeof(unsigned long *),
			&voffset, 0,
			sizes, sizeof(unsigned long *),
			cnt, ddr_phys);
	k2_measure("gather-end");
	assert(xfer);
	edma3_wait_gather(xfer);
	k2_measure("**xfer-end");

	for (p = (unsigned long *)ddr, i = 0; i < bufsize / 4; i++) {
		if (p[i] != i) {
			printf("check failed. should be %08x, actual %08x (%.2f)\n",
					i, p[i], 1.0 * i / (bufsize/4));
			for (int j = i; j < i + 128; j ++) {
				printf("%08x  ", p[j]);
				if (j % 8 == 7)
					printf("\n");
			}
			assert(0);
		}
	}
	W("check: total %d xfers all %d KB okay.", cnt, bufsize/1024);
	k2_flush_measure_format();
	free(srcs); free(sizes);
	edma3_free_channel(pp);

	W("---------- test p/v offsets --------------");
	voffset = (unsigned long)msmc - msmc_phys;
	I("virt     phys      offset  phys+offset");
	I("msmc: %08x %08x %08x %08x", msmc, msmc_phys, voffset, voffset + msmc_phys);
	voffset = (unsigned long)ddr - ddr_phys;
	I("ddr: %08x %08x %08x %08x", ddr, ddr_phys, voffset, voffset + ddr_phys);


	W("---------- test preloaded gather xfers --------------");
	I("filled src buf. size = %d KB", bufsize/1024);
	for (p = (unsigned long *)msmc, p2 = (unsigned long *)ddr, i = 0;
	        i < bufsize / 4; i++) {
		p[i] = i;
		p2[i] = 0xdeadbeef;
	}

	// start twice (test PSETs reuse)
	// each has transfers bufsize/2 in @cnt.
	cnt = 400;
	assert(bufsize/2 % cnt == 0);
	struct dma_ch_info *chinfo = edma3_allocate_channel_linked(h, cnt - 1);
	assert(chinfo);

	preload_psets(chinfo, bufsize/2/cnt);
	voffset = (unsigned long)msmc - msmc_phys;

	srcs = malloc(sizeof(unsigned long) * cnt);
	assert(srcs);

	// 1111111111111111111111111
	for (int j = 0; j < cnt; j++) {
		srcs[j] = (unsigned long)msmc + j * bufsize/2/cnt;
	}

	k2_measure("gatherp-start");
	xfer = edma3_start_gather_preloaded(chinfo,
			srcs, sizeof(unsigned long *),
			&voffset, 0,
			cnt, ddr_phys);
	k2_measure("gatherp-end");
	assert(xfer);
	edma3_wait_gather_preloaded(xfer);
	k2_measure("**xfer-end");

	// 2222222222222222222222222
	for (int j = 0; j < cnt; j++) {
		srcs[j] = (unsigned long)msmc + bufsize/2 + j * bufsize/2/cnt;
	}
	k2_measure("gatherp-start");
	xfer = edma3_start_gather_preloaded(chinfo,
			srcs, sizeof(unsigned long *),
			&voffset, 0,
			cnt, ddr_phys + bufsize/2);
	k2_measure("gatherp-end");
	assert(xfer);
	edma3_wait_gather_preloaded(xfer);
	k2_measure("**xfer-end");

	for (p = (unsigned long *)ddr, i = 0; i < bufsize / 4; i++) {
		if (p[i] != i) {
			printf("check failed. should be %08x, actual %08x (%.2f)\n",
					i, p[i], 1.0 * i / (bufsize/4));
			for (int j = i; j < i + 128; j ++) {
				printf("%08x  ", p[j]);
				if (j % 8 == 7)
					printf("\n");
			}
			assert(0);
		}
	}
	W("check: total %d xfers all %d KB okay.", 2*cnt, bufsize/1024);
	k2_flush_measure_format();

	free(srcs);
	edma3_free_channel(chinfo);

	W("---------- test preloaded gather xfer (single) --------------");

	I("filled src buf. size = %d KB", bufsize/1024);
	for (p = (unsigned long *)msmc, p2 = (unsigned long *)ddr, i = 0;
			i < bufsize / 4; i++) {
		p[i] = i;
		p2[i] = 0xdeadbeef;
	}

	int npets = 20;
	cnt = 1;
	// allocate multiple PSETs
	chinfo = edma3_allocate_channel_linked(h, npets);
	assert(chinfo);

	preload_psets(chinfo, bufsize);	// each PSET -- @bufsize
	voffset = (unsigned long)msmc - msmc_phys;

	srcs = (unsigned long)msmc;

	xfer = edma3_start_gather_preloaded(chinfo,
			&srcs, sizeof(unsigned long *),
			&voffset, 0,
			cnt, ddr_phys);
//	k2_measure("gatherp-end");
	assert(xfer);
	edma3_wait_gather_preloaded(xfer);
//	k2_measure("**xfer-end");

	for (p = (unsigned long *)ddr, i = 0; i < bufsize / 4; i++) {
		if (p[i] != i) {
			printf("check failed. should be %08x, actual %08x (%.2f)\n",
					i, p[i], 1.0 * i / (bufsize/4));
			for (int j = i; j < i + 128; j ++) {
				printf("%08x  ", p[j]);
				if (j % 8 == 7)
					printf("\n");
			}
			assert(0);
		}
	}
	W("check: preloaded gather: total %d xfers all %d KB okay.", cnt, bufsize/1024);
//	k2_flush_measure_format();
	edma3_free_channel(chinfo);

	edma3_fini();

	return 0;
}
