/*
 * cmem.cpp
 *
 *  Created on: May 28, 2015
 *      Author: xzl
 *
 *      a thin wrapper around TI's cmem api to cater to our needs.
 *
 *
 */


#include <stdio.h>
#include <sys/types.h>
#include <assert.h>
#include <stdlib.h>
#include "cmem-xzl.h"
#include "log.h"

static int is_init = 0;

/* internal */
#define MEM_TYPE_DDR 	0
#define	MEM_TYPE_MSMC 	1

const char *mt_desc[] = {"ddr", "msmc(cmem)", "msmc(mmap)", "ddr(cmem)"};

static int blk_ids[] = {-1, -1};
static const char *blk_desc[] =
    {"ddr", "msmc"};

/* init cmem; discover the blocks */
int cmem_init(void)
{
	int ret;
	int nblocks;
	CMEM_BlockAttrs blockattr;
//	CMEM_AllocParams params;
	int i;

	ret = CMEM_init();
	assert(ret == 0);

	/* check cmem blocks */
	ret = CMEM_getNumBlocks(&nblocks);
	assert(ret == 0);
	fprintf(stderr, "%s: total cmem blocks: %d\n", __func__, nblocks);

	for (i = 0; i < nblocks; i++) {
		ret = CMEM_getBlockAttrs(i, &blockattr);
		assert (ret == 0);
		fprintf(stderr, "%s: block %d: %llx @ %lx\n", __func__,
		        i, blockattr.size, blockattr.phys_base);

		if (blk_ids[MEM_TYPE_DDR] == -1 &&  is_phys_ddr(blockattr.phys_base)) {
			/* the ddr's phys base is tricky. see comments below */
			blk_ids[MEM_TYPE_DDR] = i;
		}
		if (blk_ids[MEM_TYPE_MSMC] == -1 &&  is_phys_msmc(blockattr.phys_base)) {
			blk_ids[MEM_TYPE_MSMC] = i;
		}
	}

	assert(blk_ids[MEM_TYPE_MSMC] != -1 && blk_ids[MEM_TYPE_DDR] != -1);

	fprintf(stderr, "%s: ddrblk %d msmcblk %d\n", __func__,
			blk_ids[MEM_TYPE_DDR], blk_ids[MEM_TYPE_MSMC]);

	is_init = 1;
	return 0;  // okay
}

void cmem_cleanup(void)
{
	CMEM_exit();
}

/* XXX: didn't deal with allocation size > 4g */
static void *_cmem_alloc(unsigned long bufsize, unsigned long * dma_addr, int mem_type)
{
	void *p = NULL;
//	CMEM_BlockAttrs blockattr;
	CMEM_AllocParams params;

	if (!is_init)
		cmem_init();

	int blk_id = blk_ids[mem_type];

	if (blk_id == -1) {
		assert(0);
		return NULL;
	}

	/* XXX refactor XXX */
	if (mem_type == MEM_TYPE_MSMC) {
		/* get msmc buffer */
		params.type = CMEM_HEAP;
		params.flags = CMEM_CACHED;
		params.alignment = 64; // ?
		p = CMEM_alloc2(blk_id, bufsize, &params);
		assert(p);

		//*dma_addr = CMEM_getPhys(p);
		*dma_addr = phys32_to_dma(CMEM_getPhys(p));
	} else if (mem_type == MEM_TYPE_DDR) {
		/* get ddr buffer */
		params.type = CMEM_POOL;		// CMEM_HEAP not working
#ifdef DDR_NONCACHED
		params.flags = CMEM_NONCACHED;
#else
		params.flags = CMEM_CACHED;
#endif
		params.alignment = 64; // ?
		p = CMEM_alloc2(blk_id, bufsize, &params);
		assert(p);

		/* tricky: for ddr that is above 0x8:0000:0000 (40bit phys),
		 * cmem seems to return its phys addr as the offset to 0x8:0000:0000.
		 * but edma3 expects the phys addr as (offset+0x8000:0000) (32bit bus
		 * address?)
		 * note: this does not apply msmc addr (since it is on chip?).
		 */
		//*dma_addr = (CMEM_getPhys(p)) + 0x80000000;
		*dma_addr = phys32_to_dma(CMEM_getPhys(p));
	}

	V("%s: %s allocation okay. dma_addr %lx %d KB",
			__func__, blk_desc[blk_id],
	        *dma_addr, bufsize / 1024);

	/* --- msmc --  */
	//	params.type = CMEM_HEAP;
	//	params.flags = CMEM_NONCACHED;
	//	params.alignment = 64; // ?
	//	ddr = CMEM_alloc2(1, bufsize, &params);
	//	assert(ddr);
	//	ddr_phys = CMEM_getPhys(ddr);

	return p;
}

void *cmem_alloc_ddr(unsigned long size, unsigned long * dma_addr)
{
	return _cmem_alloc(size, dma_addr, MEM_TYPE_DDR);
}

void *cmem_alloc_msmc(unsigned long size, unsigned long * dma_addr)
{
	return _cmem_alloc(size, dma_addr, MEM_TYPE_MSMC);
}
