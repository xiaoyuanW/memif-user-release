/*
 *	A simple driver exposing emif (external memory interface) hw perf counter.
 *	for TI's keystone2 SoC.
 *
 *	for memif. xzl, 2015
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <string.h>

#include "misc-xzl.h"
#include "log.h"
#include "emif.h"

// see: http://e2e.ti.com/support/dsp/c6000_multi-core_dsps/f/639/p/288732/1130574
#define DDR_MHZ	800	// this is for DDR3-1600. so that we estimate the counter overflow

static int dev_mem_fd = -1; 	// should close this afterwards
static struct emif_reg_struct * emif0 = NULL;	// XXX we only support emif0 (ddr3a)

static void *emif_mmap(uint32_t addr, uint32_t size)
{
	void *virt_addr;
	uint32_t page_size;

	if ((dev_mem_fd = open("/dev/mem", (O_RDWR | O_SYNC))) == -1) {
		W("Failed to open /dev/mem \n");
		assert(0);
	}

	page_size = sysconf(_SC_PAGE_SIZE);
	if (size%page_size) {
		W("Size does not align with page size. Size given: %d\n", size);
		return 0;
	}
	if ((uint32_t)addr % page_size) {
		W("Address does not align with page size. Address given: 0x%08x\n", (uint32_t) addr);
		return 0;
	}
	virt_addr = mmap(0, size, (PROT_READ|PROT_WRITE), MAP_SHARED, dev_mem_fd, (off_t)addr);
	if (virt_addr == (void *)-1) {
		fprintf(stderr, "mmap failed!\n");
		assert(0);
		return 0;
	}
	I("xzl: %s: %08x --> %08x", __func__, virt_addr, addr);
	return virt_addr;
}

static void emif_config(void)
{
	uint32_t ev1, ev2; // the perf events that we are going to count
	assert(emif0);

	ev1 = 0xa;	// emif busy cycles
	ev2 = 0xb; 	// ECC errors

	// XXX should use EMIF_REG_CNTR2_CFG_SHIFT etc.
	emif0->emif_perf_cnt_cfg |= ((ev2 << 16) | (ev1 << 0));
}

// return 0 on sucess
int emif_perfcnt_init(void)
{
	assert(!emif0);
	emif0 = (struct emif_reg_struct *)emif_mmap(EMIF1_BASE, SZ_4K);
	assert(emif0);

	emif_config();
	return 0;
}

static struct timeval start;
// a snapshot of regs
static uint32_t start_cnt1, start_cnt2, start_cycles;

// this function is non reentrant
void emif_perfcnt_start(void)
{
	if (!emif0)
		emif_config();

	gettimeofday(&start, NULL);
	start_cycles = emif0->emif_perf_cnt_tim;
	start_cnt1 	= emif0->emif_perf_cnt_1;
	start_cnt2 	= emif0->emif_perf_cnt_2;
}

void emif_perfcnt_end(void)
{
	struct timeval end;
	uint32_t cnt1, cnt2, cycles, elapsed_cycles;
	uint32_t us;

	cycles = emif0->emif_perf_cnt_tim;
	cnt1 	= emif0->emif_perf_cnt_1;
	cnt2 	= emif0->emif_perf_cnt_2;
	cnt2 = cnt2; // suppress compiler warning. XXX use cnt2.
	gettimeofday(&end, NULL);

	us = (end.tv_sec * 1000000 + end.tv_usec)
		  - (start.tv_sec * 1000000 + start.tv_usec);

	// are we seeing an overflow?
	if (us > 0xffffffff / (DDR_MHZ/2)
			|| cycles < start_cycles) {
		W("Warning: ddr cycle counter overflow. Use system clock instead");
		elapsed_cycles = (DDR_MHZ/2) * us;
	} else {
		W("elapsed time: %u us (ddr cycle couter: %u us)",
				us, (cycles - start_cycles) / (DDR_MHZ/2));
		elapsed_cycles = cycles - start_cycles;
	}

	if (cnt1 < start_cnt1) {
		W("ddr cnt1 overflow");
	} else {
		W("busy cycles: %u. utilization %.4f", cnt1-start_cnt1,
				(cnt1-start_cnt1)*1.0/(elapsed_cycles));
	}
}

#ifdef UNITTEST
int main(int argc, char **argv)
{
	struct emif_reg_struct * emif0 = emif_mmap(EMIF1_BASE,
			4096);
	assert(emif0);

	// check the rev...
	I("rev %08x", emif0->emif_mod_id_rev);

	// check addr offset
	I("emif_perf_cnt_tim %08x\n",
			&(((struct emif_reg_struct *)0)->emif_perf_cnt_tim));

	// read from the reg;
	I("cnt time %08x", emif0->emif_perf_cnt_tim);
	I("cnt time %08x", emif0->emif_perf_cnt_tim);
	I("cnt time %08x", emif0->emif_perf_cnt_tim);

	uint32_t type1, type2; // event type to count
	type1 = 0xa; 	// DDR i/f busy cycles
	type2 = 0xb; 	// ECC errors

	// XXX should use EMIF_REG_CNTR2_CFG_SHIFT etc.
	emif0->emif_perf_cnt_cfg |= ((type2 << 16) | (type1 << 0));

	I("cnt1 %08x", emif0->emif_perf_cnt_1);
	I("cnt1 %08x", emif0->emif_perf_cnt_1);
	I("cnt1 %08x", emif0->emif_perf_cnt_1);

	I("cnt2 %08x", emif0->emif_perf_cnt_2);
	I("cnt2 %08x", emif0->emif_perf_cnt_2);
	I("cnt2 %08x", emif0->emif_perf_cnt_2);

}
#endif 	// UNITTEST
