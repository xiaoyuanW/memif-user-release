/*
 * edma3.cpp
 *
 *  Created on: May 27, 2015
 *      Author: xzl
 *
 *  A wrapper/test of TI's userspace edma3 driver (in libedma3)
 *  mostly retrofit from edma3-lld test file.
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
#include <semaphore.h>
//#include "edma3_drv.h"
#include <ti/sdo/edma3/drv/edma3_drv.h>  // xzl

#define K2_NO_MEASUREMENT 	1 // disable measurement. has to come before measure.h
#include "measure.h"
#include "edma3-xzl.h"
#include "log.h"

#ifdef DEBUG
#define debug_printf(fmt, args...) printf(fmt, args...)
#else
#define debug_printf(fmt, args...)
#endif

/* OPT Field specific defines */
#define OPT_SYNCDIM_SHIFT                   (0x00000002u)
#define OPT_TCC_MASK                        (0x0003F000u)
#define OPT_TCC_SHIFT                       (0x0000000Cu)
#define OPT_TCINTEN_SHIFT                   (0x00000014u)	//20 complete irq
#define OPT_ITCINTEN_SHIFT                  (0x00000015u)	//21 intermediate complete irq
/* xzl: more. avoid using the api */
#define OPT_TCCHEN_SHIFT	22		// xfer complete chaining
#define OPT_ITCCHEN_SHIFT 	23		// xfer intermediate complete chaining

/* handy macro */
#define checkret() assert(result == EDMA3_DRV_SOK)

static void dump_chinfo(struct dma_ch_info *chinfo);

#if 0 // xzl: userspace edma driver uses polling only. no irq (callback) is delivered.
/* Flag variable to check transfer completion on channel 1 */
volatile short irqRaised1 = 0;
/* Flag variable to check transfer completion on channel 2 */
volatile short irqRaised2 = 0;
#endif

/* internal: used to describe an outstanding dma xfer */
struct edma_xfer
{
	EDMA3_DRV_Handle hEdma;
	unsigned int chId;	// we don't need chId for wait(). example code bug.
	unsigned int tcc;
	unsigned long size;
	struct timeval start;
};

struct edma_xfer_scatter
{
	struct dma_ch_info *chinfo;
	int cnt; // needed?
	int totalsize;	// needed?
	struct timeval start;
};

/**
 * Shadow Region on which the executable is runnig. Its value is populated
 * with the DSP Instance Number here in this case.
 */
static unsigned int region_id;

extern EDMA3_DRV_GblConfigParams sampleEdma3GblCfgParams[];
extern EDMA3_DRV_InstanceInitConfig sampleInstInitConfig[][EDMA3_MAX_REGIONS];

static sem_t mutex;
static int dev_mem_fd;

/* Callback function 1 -- called when xfer completes
 *
 * */
void callback1(unsigned int tcc, EDMA3_RM_TccStatus status, void *appData)
{
	(void) tcc;
	(void) appData;
#if 0

	switch (status) {
	case EDMA3_RM_XFER_COMPLETE:
		/* Transfer completed successfully */
		irqRaised1 = 1;
		break;
	case EDMA3_RM_E_CC_DMA_EVT_MISS:
		/* Transfer resulted in DMA event miss error. */
		irqRaised1 = -1;
		break;
	case EDMA3_RM_E_CC_QDMA_EVT_MISS:
		/* Transfer resulted in QDMA event miss error. */
		irqRaised1 = -2;
		break;
	default:
		break;
	}
#endif
}

#if 0  // not useful...
void callback_lnk(unsigned int tcc, EDMA3_RM_TccStatus status, void *appData)
{
	struct edma_xfer *xfer = (struct edma_xfer *)appData;

	I("callback_lnk");

	switch (status) {
	case EDMA3_RM_XFER_COMPLETE:
		/* Transfer completed successfully */
		irqRaised1 = 1;
		EDMA3_DRV_enableTransfer(xfer->hEdma, xfer->chId,
		                         EDMA3_DRV_TRIG_MODE_MANUAL);
		I("callback_lnk: one param set complete");
		break;
	case EDMA3_RM_E_CC_DMA_EVT_MISS:
		/* Transfer resulted in DMA event miss error. */
		assert(0);
		irqRaised1 = -1;
		break;
	case EDMA3_RM_E_CC_QDMA_EVT_MISS:
		/* Transfer resulted in QDMA event miss error. */
		assert(0);
		irqRaised1 = -2;
		break;
	default:
		break;
	}
}
#endif

static uint32_t edma3_mmap(uint32_t addr, uint32_t size)
{
	uint32_t virt_addr;
	uint32_t page_size;
	page_size = sysconf(_SC_PAGE_SIZE);
	if (size%page_size) {
		debug_printf("Size does not align with page size. Size given: %d\n", size);
		return 0;
	}
	if ((uint32_t)addr % page_size) {
		debug_printf("Address does not align with page size. Address given: 0x%08x\n", (uint32_t) addr);
		return 0;
	}
	virt_addr = (uint32_t) mmap(0, size, (PROT_READ|PROT_WRITE), MAP_SHARED, dev_mem_fd, (off_t)addr);
	if (virt_addr == -1) {
		fprintf(stderr, "mmap failed!\n");
		assert(0);
		return 0;
	}
	I("xzl: %s: %08x --> %08x", __func__, virt_addr, addr);
	return virt_addr;
}

static void dump_paramset(EDMA3_DRV_PaRAMRegs *set)
{
	assert(set);
	printf("aCnt    bCnt	cCnt	destAddr	linkAddr\n");
	printf("%04x %04x %04x %08x %04x\n", set->aCnt, set->bCnt, set->cCnt,
	       set->destAddr, set->linkAddr);
}

/* xzl: this rewrites the phys addrs with virt addrs (obtained through mmap).
 *
 * note: the sample config values only provide the regs of TC 0/1.
 * see sampleEdma3GblCfgParams in evmTCl...Sample.c */
static EDMA3_DRV_GblConfigParams g_phys_cfg;	// we keep a copy (in case need phys global/tcRegs)

static int setupEdmaConfig(EDMA3_DRV_GblConfigParams *cfg)
{
	int i;
	assert(cfg);
	memcpy(&g_phys_cfg, cfg, sizeof(g_phys_cfg)); // keep a copy

	cfg->globalRegs = (uint32_t *)edma3_mmap((uint32_t)cfg->globalRegs, 0x8000);
	for (i = 0; i < EDMA3_MAX_TC; i++) {
		if (cfg->tcRegs[i] != NULL)
			cfg->tcRegs[i] =(uint32_t *)edma3_mmap((uint32_t)cfg->tcRegs[i], 0x1000);
	}

	// seems 4001ab00 (evmk2h)?
	printf("%s: trying to read REV... %08x\n", __func__,
			*((unsigned int*)(cfg->globalRegs)));
	return 1;
}

// a wrapper over the lld EDMA3_DRV_getPaRAMPhyAddr(), which mistakely return virt addr.
// return: non-zero phys addr on success. otherwise 0
static unsigned long edma3_get_param_phys(EDMA3_DRV_Handle hdma, int chid)
{
	EDMA3_DRV_Result result;
	unsigned long param_virt;
	// this was the phys addr but should have been overwritten with virt
	// addr by setupEdmaConfig()
	unsigned long regbase_virt = (unsigned long)sampleEdma3GblCfgParams[0].globalRegs;
	unsigned long regbase_phys = (unsigned long)g_phys_cfg.globalRegs;

	//	I(" -------------> regbase_virt %08x", regbase_virt);
	assert((unsigned long)regbase_virt < 0xc0000000); 	// sanity check. has to be a virt addr

	// obtained the param set *virt* addr (mmap'd)
	result = EDMA3_DRV_getPaRAMPhyAddr(hdma, chid, (uint32_t *)(&param_virt));

	if (result != EDMA3_DRV_SOK) {
		assert(0 && "get param failed");
		return 0;
	}

	// do a reverse lookup
	// the param set is within edma3's global reg space
	if (param_virt < regbase_virt
	        && param_virt >= regbase_virt + 0x8000) {
		assert(0 && "phys out of mapp'd range");
		return 0;
	}

	assert(regbase_phys);
	assert(regbase_phys < regbase_virt && "not implemented");

	return param_virt - regbase_virt + regbase_phys;
}

EDMA3_DRV_Handle edma3_init(void)
{
	EDMA3_DRV_Result edma3Result = EDMA3_DRV_E_INVALID_PARAM;
	EDMA3_DRV_GblConfigParams *globalConfig = NULL;
	EDMA3_DRV_InstanceInitConfig *instanceConfig = NULL;
	EDMA3_DRV_InitConfig initCfg;
	EDMA3_RM_MiscParam miscParam;
	EDMA3_DRV_Handle hEdma = NULL;
	int edma3Id = 0;	// edma phys instance 0
	if ((dev_mem_fd = open("/dev/mem", (O_RDWR | O_SYNC))) == -1) {
		debug_printf("Failed to open /dev/mem \n");
		assert(0);
		return (EDMA3_DRV_Handle)0; /* XXX */
	}

	//xzl: use the predef config (intended for instance0)
	globalConfig = &sampleEdma3GblCfgParams[0];
	setupEdmaConfig(globalConfig);
	edma3Result = EDMA3_DRV_create(edma3Id, globalConfig, (void *) &miscParam);
	/* configuration structure for the Driver.
	 * 0: load predefined region config 0 */
	instanceConfig = &sampleInstInitConfig[edma3Id][0];

	initCfg.isMaster = TRUE;
	/* Choose shadow region according to the DSP# */
	initCfg.regionId = (EDMA3_RM_RegionId)0;
	/*Saving the regionId for using it in the sample_cs.c file */
	region_id = (EDMA3_RM_RegionId)0;
	/* Driver instance specific config NULL */
	initCfg.drvInstInitConfig = instanceConfig;
	initCfg.gblerrCb = NULL;
	initCfg.gblerrData = NULL;
	initCfg.drvSemHandle = (void *)0xdeadbeef; /* xzl: EDMA3_DRV_open() will complain */

	/* Open the Driver Instance
	 * xzl: @initCfg carrys one shadow region (specified by @drvInstInitConfig)
	 * for use. note that @initCfg.regionId specifies region id */
	hEdma = EDMA3_DRV_open(edma3Id, (const EDMA3_DRV_InitConfig *) &initCfg,
	                       &edma3Result);
	if (hEdma)
		printf("hEdma opened!\n");
	else {
		printf("edma open failed! edma3Result=%d\n", edma3Result);
		assert(0);
	}
	if (edma3Result == EDMA3_DRV_SOK)
		printf("edmaResult is ok\n");
	else {
		printf("edma open result not okay\n");
		assert(0);
	}

	/* reconfigure TC priority -- all set to be lowest
	 * https://e2e.ti.com/support/dsp/davinci_digital_media_processors/f/99/t/7400
	 * */
	int i, result;
	int prio = 7;		/* 0: highest; 7: lowest */
	EDMA3_DRV_EvtQuePriority priorityMy;
	for(i=0; i<8; i++)
		priorityMy.evtQPri[i] = prio;
	result = EDMA3_DRV_setEvtQPriority(hEdma, &priorityMy);
	checkret();
	I("configure all TCs' priorities to be %d ", prio);

	/* leave dev_mem_fd open */

	return hEdma;
}

EDMA3_DRV_Result edma3_fini(void)
{
	close(dev_mem_fd);
	return EDMA3_DRV_SOK;
}

// Repeat the same xfer multiple times.
// (in LLD's term) this alloc one phys channels + many logic channels
// (which are actually paramSets).
//
// Due to the virt/phys addr glitch in the stock LLD driver, we have to
// manually link PSETs.
//
// irq raised after each xfer.
// return 0 on success
int edma3_start_xfer_multiple(EDMA3_DRV_Handle hEdma, unsigned long src,
                              unsigned long dest, unsigned long size, int count, void **xfer)
{
	EDMA3_DRV_Result result = EDMA3_DRV_SOK;
	EDMA3_DRV_PaRAMRegs paramSet = {0,0,0,0,0,0,0,0,0,0,0,0};
	unsigned int chId = 0;
	unsigned int * logic_chs = NULL;  // an array of logic channels
	unsigned int prev_ch; //xzl: used in linking channels.
	unsigned int tcc = 0;
	int i;
	//	unsigned int numenabled = 0;
	unsigned int BRCnt = 0;
	int acnt = 0, bcnt = 0, ccnt = 1;
	int srcbidx = 0, desbidx = 0;
	int srccidx = 0, descidx = 0;
	int max_narrays = SZ_32K;

	//	char *src_virt, *dest_virt;  	/* need them to flush/inv cache & verify results */

	int syncType;
	const char *syncdesc;

	/* the xfer desc. used to wait for xfer results */
	*xfer = (struct edma_xfer *)malloc(sizeof(struct edma_xfer));
	assert(*xfer);
	//	struct edma_xfer *exfer = (struct edma_xfer *)(*xfer); // handy

	/* setting the xfer size. use AB sync that can xfer up to 4096M.
	 *
	 * the cpu should trigger (''sync'') once for the entire transfer.
	 * thus, we choose A sync or AB sync properly.
	 * XXX: we didn't check size > 1 frame (4gig)
	 * */

	/* 64k if single array, otherwise 32k (bounded by bidx) */
#define MAX_ARRAY_SIZE_SINGLE  	SZ_64K
	#define MAX_ARRAY_SIZE_MULTI	SZ_32K

	if (size < MAX_ARRAY_SIZE_SINGLE) {	/* can fit in one array */
		syncType = EDMA3_DRV_SYNC_A;
		syncdesc = "Sync A";
		acnt = size;
		bcnt = 1;
	} else { /* can't fit in one array. how many arrays do we need? */
		int narrays = 1;
		syncType = EDMA3_DRV_SYNC_AB;
		syncdesc = "Sync AB";
		bcnt = acnt = -1;
		while (narrays <= max_narrays) {
			narrays *= 2;
			if (size % narrays != 0 || size / narrays >= MAX_ARRAY_SIZE_MULTI)
				continue;
			bcnt = narrays;
			acnt = size / narrays;
			break;
		}

		if (bcnt == -1) {
			fprintf(stderr, "%s:not impl: can't evenly distribute %lu bytes into arrays.\n",
			        __func__, size);
			assert(0);
			return EDMA3_DRV_E_INVALID_PARAM;
		}
	}

	/* Set B count reload as B count. */
	BRCnt = bcnt;	/* unused for AB sync */
	srcbidx = (int) acnt;
	desbidx = (int) acnt;

	if (syncType == EDMA3_DRV_SYNC_A) {
		/* A Sync Transfer Mode */
		srccidx = (int) acnt;
		descidx = (int) acnt;
	} else {
		/* AB Sync Transfer Mode */
		srccidx = ((int) acnt * (int) bcnt);
		descidx = ((int) acnt * (int) bcnt);
	}

	/* sanity check indices. when ccnt==1, the xfer is done in one burst,
	 * making cidx irrelevant. same for bcnt. */
	if (bcnt > 1)
		assert(srcbidx <= 32767 && desbidx <= 32767);
	if (ccnt > 1)
		assert(srccidx <= 32767 && descidx <= 32767);

	D("xzl:%s: mode: %s acnt %d bcnt %d ccnt %d\n", __func__,
	  syncdesc, acnt, bcnt, ccnt);

	/* Setup for channel 1 (the physical channel, or called "master" channel in some example) */
	tcc = EDMA3_DRV_TCC_ANY;
	chId = EDMA3_DRV_DMA_CHANNEL_ANY;	// request a physical dma channel

	assert(result == EDMA3_DRV_SOK);

	result = EDMA3_DRV_requestChannel(hEdma, &chId, &tcc,
	                                  (EDMA3_RM_EventQueue) 0, &callback1, *xfer);
	assert(result == EDMA3_DRV_SOK);

	I("requested channel: chid %d (tcc %d)", chId, tcc);

	/* --------------------------------------------------
	 * Fill the PaRAM Set with transfer specific information
	 * --------------------------------------------------*/
	paramSet.srcAddr = src;
	paramSet.destAddr = dest;

	/**
	 * Be Careful !!!
	 * Valid values for SRCBIDX/DSTBIDX are between .32768 and 32767
	 * Valid values for SRCCIDX/DSTCIDX are between .32768 and 32767
	 */
	paramSet.srcBIdx = srcbidx;
	paramSet.destBIdx = desbidx;
	paramSet.srcCIdx = srccidx;
	paramSet.destCIdx = descidx;

	/**
	 * Be Careful !!!
	 * Valid values for ACNT/BCNT/CCNT are between 0 and 65535.
	 * ACNT/BCNT/CCNT must be greater than or equal to 1.
	 * Maximum number of bytes in an array (ACNT) is 65535 bytes
	 * Maximum number of arrays in a frame (BCNT) is 65535
	 * Maximum number of frames in a block (CCNT) is 65535
	 */
	paramSet.aCnt = acnt;
	paramSet.bCnt = bcnt;
	paramSet.cCnt = ccnt;

	/* For AB-synchronized transfers, BCNTRLD is not used. */
	paramSet.bCntReload = BRCnt;

	paramSet.linkAddr = 0xFFFFu;

	/* Src & Dest are in INCR modes */
	paramSet.opt &= 0xFFFFFFFCu;
	/* Program the TCC */
	paramSet.opt |= ((tcc << OPT_TCC_SHIFT) & OPT_TCC_MASK);

	/* Enable Intermediate & Final transfer completion interrupt.
	 * XXX: turn off intermediate irq? */
	paramSet.opt |= (1 << OPT_ITCINTEN_SHIFT);
	//paramSet.opt &= ~(1 << OPT_ITCINTEN_SHIFT);
	paramSet.opt |= (1 << OPT_TCINTEN_SHIFT);

	if (syncType == EDMA3_DRV_SYNC_A) {
		paramSet.opt &= 0xFFFFFFFBu;
	} else {
		/* AB Sync Transfer Mode */
		paramSet.opt |= (1 << OPT_SYNCDIM_SHIFT);
	}

	/* Now, write the PaRAM Set. */
	result = EDMA3_DRV_setPaRAM(hEdma, chId, &paramSet);
	assert(result == EDMA3_DRV_SOK);


	/* ------------------------------------
	 * Grab (N-1) param sets and link them ("logic channels" in LLD's lingo)
	 *
	 * Note: Don't use EDMA3_DRV_linkChannel() or EDMA3_DRV_getPaRAMPhyAddr(),
	 * which mistakely treat the virt addr as phys addr.
	 *
	 * tcc: only one tcc is needed for all xfers.
	 *
	 * triggering: since this is linking, the CPU needs to trigger EDMA3 after
	 * every PSET is completed.
	 * ----------------------------------- */
	unsigned short param_id, prev_param_id;
	unsigned int phys_addr;

	phys_addr = edma3_get_param_phys(hEdma, chId);
	D("edma phys %08x", phys_addr);
	param_id = (unsigned short)(phys_addr & 0xffff);
	prev_param_id =  param_id;
	D("*** ch %d param %08x", chId, param_id);

	logic_chs = malloc(sizeof(int) * (count - 1));
	assert(logic_chs);

	prev_ch = chId;

	for (i = 0; i < count-1; i++) {
		logic_chs[i] = EDMA3_DRV_LINK_CHANNEL;

		result = EDMA3_DRV_requestChannel(hEdma, &logic_chs[i], &tcc,
		                                  (EDMA3_RM_EventQueue) 0, &callback1, *xfer);
		assert(result == EDMA3_DRV_SOK);

		/* reuse the param set from the previous phys/logic channel. */

		// load previous param set, this internally just does memcpy
		result = EDMA3_DRV_getPaRAM(hEdma, prev_ch, &paramSet);
		assert(result == EDMA3_DRV_SOK);

		phys_addr = edma3_get_param_phys(hEdma, logic_chs[i]);
		D("edma phys %08x", phys_addr);
		param_id = (unsigned short)(phys_addr & 0xffff);

		// set: prev->linkAddr = param_id
		paramSet.linkAddr = param_id;
		I("----> after linking, param %08x, linkAddr %08x", prev_param_id,
		  paramSet.linkAddr);
		// (re-)write the previous PSET
		result = EDMA3_DRV_setPaRAM(hEdma, prev_ch, &paramSet);
		assert(result == EDMA3_DRV_SOK);

		// program the current PSET, leaving link to NULL
		paramSet.linkAddr = 0xffff;
		result = EDMA3_DRV_setPaRAM(hEdma, logic_chs[i], &paramSet);

		prev_ch = logic_chs[i];
		prev_param_id = param_id;
	}

	/* ------------------------------------
	 * Kick channel 1 (the phys one) to start xfer
	 * ----------------------------------- */
	int kickcnt = 0;
	D("kick %d", kickcnt++);
	result = EDMA3_DRV_enableTransfer(hEdma, chId,
	                                  EDMA3_DRV_TRIG_MODE_MANUAL);
	assert(result == EDMA3_DRV_SOK);

	D("start to wait...tcc %d", tcc);
	result = EDMA3_DRV_waitAndClearTcc(hEdma, tcc);	 // xzl: should use tcc (not chId)
	assert(result == EDMA3_DRV_SOK);
	D("wait done...");

	// optional: examining the auto-loaded param set
	phys_addr = edma3_get_param_phys(hEdma, chId);
	param_id = (unsigned short)(phys_addr & 0xffff);
	result = EDMA3_DRV_getPaRAM(hEdma, chId, &paramSet);
	assert(result == EDMA3_DRV_SOK);
	I("1: PSET %08x linkAddr %08x", param_id, paramSet.linkAddr);

	// then we have to kick the _phys_ channel @count-1 times
	for (i = 0; i < count-1; i++) {
		D("kick %d", kickcnt++);
		result = EDMA3_DRV_enableTransfer(hEdma, chId,
		                                  EDMA3_DRV_TRIG_MODE_MANUAL);
		assert(result == EDMA3_DRV_SOK);

		// Only has to wait for the phys channel's tcc
		D("start to wait: tcc %d", tcc);
		result = EDMA3_DRV_waitAndClearTcc(hEdma, tcc);
		assert(result == EDMA3_DRV_SOK);
		D("wait done...");

		// after the last one, a zero'd PSET will be loaded
		phys_addr = edma3_get_param_phys(hEdma, chId);
		param_id = (unsigned short)(phys_addr & 0xffff);
		paramSet.linkAddr = 0;  // clean it to make sure we're loading value
		result = EDMA3_DRV_getPaRAM(hEdma, chId, &paramSet);
		I("%d: PSET %08x linkAddr %08x", i + 2, param_id, paramSet.linkAddr);
	}

	I("all %d xfer done\n", i+1);

	// cleanup
	result = EDMA3_DRV_freeChannel(hEdma, chId);
	assert(result == EDMA3_DRV_SOK);

	for (i = 0; i < count-1; i++) {
		result = EDMA3_DRV_freeChannel(hEdma, logic_chs[i]);
		assert(result == EDMA3_DRV_SOK);
	}

	return 0;
}


// return 0 on success. otherwise fail
static int get_xfer_geometry(unsigned long size, int *acnt, int *bcnt, int *ccnt,
                             int *bidx, int *cidx, int *brcnt, int *syncType)
{
	*ccnt = 1;
	const char *syncdesc;

	/* "array" -- A xfer */
	int max_narrays = SZ_32K;	// in theory can go up to 65535

	/* the cpu should trigger (''sync'') once for the entire transfer.
	* thus, we choose A sync or AB sync properly.
	* XXX: we didn't check size > 1 frame (4gig)
	*/

	/* a single array can hold < 64k;
	 * if we have to use multiple arrays, each array is
	 * has to be < 32k (bounded by bidx) */
#define MAX_ARRAY_SIZE_SINGLE  	SZ_64K
#define MAX_ARRAY_SIZE_MULTI	SZ_32K

	if (size < MAX_ARRAY_SIZE_SINGLE) {	/* can fit in one array */
		*syncType = EDMA3_DRV_SYNC_A;
		syncdesc = "Sync A";
		*acnt = size;
		*bcnt = 1;
	} else {
		/* can't fit in one array. how many arrays do we need?
		 * we search, hoping to use arrays as few as possible. */
		int narrays = 1;
		*syncType = EDMA3_DRV_SYNC_AB;
		syncdesc = "Sync AB";
		*bcnt = *acnt = -1;
		while (narrays <= max_narrays) {
			narrays *= 2;
			if (size % narrays != 0 || size / narrays >= MAX_ARRAY_SIZE_MULTI)
				continue;
			*bcnt = narrays;
			*acnt = size / narrays;
			break;
		}

		if (*bcnt == -1) {
			fprintf(stderr, "%s:not impl: can't evenly distribute %lu bytes into arrays.\n",
			        __func__, size);
			assert(0);
			return -1;
		}
	}

	D("mode %s", syncdesc);

	/* Set B count reload as B count. */
	*brcnt = *bcnt;	/* unused for AB sync */
	*bidx =  *acnt;

	if (*syncType == EDMA3_DRV_SYNC_A) {
		/* A Sync Transfer Mode */
		*cidx = *acnt;
	} else {
		/* AB Sync Transfer Mode */
		*cidx = (*acnt) * (*bcnt);
	}

	/* sanity check indices. note indices are signed values from HW's POV.
	 * when ccnt==1, the xfer is done in one burst,
	 * making cidx irrelevant. same for bcnt. */
	if (*bcnt > 1)
		assert(*bidx <= 32767);
	// otherwise bidx does not matter

	if (*ccnt > 1)
		assert(*cidx <= 32767);
	// otherwise cidx does not matter

	return 0;
}

// given @size of each xfer, load all PSETs of a channel with default values
// return 0 on success
int preload_psets(struct dma_ch_info * chinfo, unsigned long size)
{
	assert(chinfo);
	assert(!chinfo->pset); 	// never preloaded before

	int BRCnt = 0;
	int acnt = 0, bcnt = 0, ccnt = 1;
	int srcbidx = 0;
	int srccidx = 0;
	int synctype;
	EDMA3_DRV_PaRAMRegs *paramSet = malloc(sizeof(EDMA3_DRV_PaRAMRegs));
	assert(paramSet);

	bzero(paramSet, sizeof(EDMA3_DRV_PaRAMRegs));	// clear all fields

	get_xfer_geometry(size, &acnt, &bcnt, &ccnt, &srcbidx, &srccidx,
	                  &BRCnt, &synctype);

	// ------- fill the fields that are same in all PSETs ----
	paramSet->srcBIdx = srcbidx;
	paramSet->destBIdx = srcbidx;	// src/dest same idx
	paramSet->srcCIdx = srccidx;
	paramSet->destCIdx = srccidx;	// src/dest same idx

	paramSet->aCnt = acnt;
	paramSet->bCnt = bcnt;
	paramSet->cCnt = ccnt;
	/* For AB-synchronized transfers, BCNTRLD is not used. */
	paramSet->bCntReload = BRCnt;

	/* sync mode */
	if (synctype == EDMA3_DRV_SYNC_A)
	{
		paramSet->opt &= 0xFFFFFFFBu;
	} else
	{ /* AB */
		paramSet->opt |= (1 << OPT_SYNCDIM_SHIFT);
	}

	/* Src & Dest are in INCR modes */
	paramSet->opt &= 0xFFFFFFFCu;
	/* Program the TCC -- since we are chaining,
	 * this: 1) chain to itself for xfers 0..n-2
	 * 2) set tcc irq code for xfer n-1 */
	paramSet->opt &= (~(OPT_TCC_MASK));
	paramSet->opt |= ((chinfo->tcc << OPT_TCC_SHIFT) & OPT_TCC_MASK);
	// in theory, the last PSET's tcc should be really @tcc instead of
	// phys chId. we assume...
	assert(chinfo->tcc == chinfo->ch);

	/* no intermediate complete irq */
	paramSet->opt &= ~(1 << OPT_ITCINTEN_SHIFT);
	/* no complete irq */
	paramSet->opt &= ~(1 << OPT_TCINTEN_SHIFT);
	/* yes complete chaining */
	paramSet->opt |= (1 << OPT_TCCHEN_SHIFT);
	/* no intermediate complete chaining */
	paramSet->opt &= ~(1 << OPT_ITCCHEN_SHIFT);

	// ----- leave src/dest/link as empty -----
	int i;
	EDMA3_DRV_Result result = EDMA3_DRV_SOK;

	// phys channel
	// also okay: EDMA3_DRV_setPaRAMEntry(hEdma, chinfo->ch, chinfo->link);
	paramSet->linkAddr = chinfo->link;
	result = EDMA3_DRV_setPaRAM(chinfo->hedma, chinfo->ch, paramSet);
	checkret();

	// all logic channels
	for (i = 0; i < chinfo->logic_cnt; i++)
	{
		paramSet->linkAddr = chinfo->links[i];
		result = EDMA3_DRV_setPaRAM(chinfo->hedma, chinfo->logic_chs[i], paramSet);
		checkret();
	}

	// clear this
	paramSet->linkAddr = 0;
	chinfo->pset = paramSet;
	chinfo->size = size;

	// precompute some of the last PSET's parameters
	chinfo->last_brcnt_link = ((BRCnt << 16) | 0xffff);

	chinfo->last_opt = paramSet->opt;
	/* no intermediate complete irq */
	chinfo->last_opt &= ~(1 << OPT_ITCINTEN_SHIFT);
	/* yes complete irq */
	chinfo->last_opt |= (1 << OPT_TCINTEN_SHIFT);
	/* no complete chaining */
	chinfo->last_opt &= ~(1 << OPT_TCCHEN_SHIFT);
	/* no intermediate complete chaining */
	chinfo->last_opt &= ~(1 << OPT_ITCCHEN_SHIFT);

	return 0;
}

/* @handle: channel handle, a pointer returned by
 * edma3_allocate_channel_linked()
 *
 * @srcs, @src_stride: specifying the source buffers.
 *
 * @srcs: the addr of the _virt_ pointer to the 1st source buffer,
 * 	the next pointer is @src_stride away
 *
 * the user should ensure @dest buffer has enough room.
 *
 * return: xfer handle */
struct edma_xfer_scatter *edma3_start_gather(void *handle, unsigned long *srcs, int src_stride,
			        unsigned long *voffsets, int voffset_stride,
			        unsigned long *sizes, int size_stride,
			        unsigned long cnt, unsigned long dest)
{
	struct dma_ch_info *chinfo = handle;
	assert(handle);
	EDMA3_DRV_Handle hEdma = chinfo->hedma;
	unsigned int phys_chId = chinfo->ch;
	unsigned int tcc = chinfo->tcc;

	int BRCnt = 0;
	int acnt = 0, bcnt = 0, ccnt = 1;
	int srcbidx = 0, desbidx = 0;
	int srccidx = 0, descidx = 0;
	int synctype;
	unsigned long prev_size = 0;

	unsigned long dest_off = 0;  // offset in dest buffer

	EDMA3_DRV_Result result = EDMA3_DRV_SOK;
	EDMA3_DRV_PaRAMRegs paramSet = {0,0,0,0,0,0,0,0,0,0,0,0};

	/* sanity chk */
	assert(src_stride == 0 || src_stride >= sizeof(unsigned long *));
	assert(size_stride == 0 || size_stride >= sizeof(unsigned long *));
	assert(voffset_stride == 0 || voffset_stride >= sizeof(unsigned long *));

	/* the xfer desc. used to wait for xfer results */
	struct edma_xfer_scatter *xfer = \
				                                 (struct edma_xfer_scatter *)malloc(sizeof(struct edma_xfer_scatter));
	assert(xfer);

	assert(chinfo->logic_cnt + 1 >= cnt);  // we have enough PSETs preallocated

	// go through each src/size to populate PSETs
	// i is the number of xfer (PSET), including the phys one.
	unsigned long *psrc = srcs;   // pointer to the buffer pointer
	unsigned long *pvoffset = voffsets;
	unsigned long *psz = sizes;
	unsigned long src, voffset, size; // src: buffer's dma addr

	k2_measure("start_gather:inited");

	for (int i = 0; i < cnt; i++)
{
		int chid = ((i == 0) ? phys_chId : chinfo->logic_chs[i - 1]);

		voffset = *pvoffset;
		assert(voffset > 0);
		size = *psz;
		src = *psrc - voffset;	// *psrc: the buffer virt addr. convert to dma addr

		psrc = (unsigned long *)((unsigned long) psrc + src_stride); // find the next buffer pointer
		pvoffset = (unsigned long *)((unsigned long) pvoffset + voffset_stride);
		psz = (unsigned long *)((unsigned long) psz + size_stride);

		// --------------- geometry ---------------------
		if (size != prev_size) {
			get_xfer_geometry(size, &acnt, &bcnt, &ccnt, &srcbidx, &srccidx,
			                  &BRCnt, &synctype);
			desbidx = srcbidx;
			descidx = srccidx;

			// double chk. shouldn't be necessary
			if (bcnt > 1)
				assert(srcbidx <= 32767 && desbidx <= 32767);
			if (ccnt > 1)
				assert(srccidx <= 32767 && descidx <= 32767);

			V("xzl:%s: acnt %d bcnt %d ccnt %d", __func__,
			  acnt, bcnt, ccnt);

			// load geometry into PSET
			paramSet.srcBIdx = srcbidx;
			paramSet.destBIdx = desbidx;
			paramSet.srcCIdx = srccidx;
			paramSet.destCIdx = descidx;

			paramSet.aCnt = acnt;
			paramSet.bCnt = bcnt;
			paramSet.cCnt = ccnt;

			paramSet.bCntReload = BRCnt;

			/* For AB-synchronized transfers, BCNTRLD is not used. */
			paramSet.bCntReload = BRCnt;

			/* sync mode */
			if (synctype == EDMA3_DRV_SYNC_A) {
				paramSet.opt &= 0xFFFFFFFBu;
			} else { /* AB */
				paramSet.opt |= (1 << OPT_SYNCDIM_SHIFT);
			}
		} else {
			assert(i != 0);
			// reuse previously calculated PSET
		}

		// update src/dest
		paramSet.srcAddr = src;
		paramSet.destAddr = dest + dest_off;
		dest_off += size;

		// ------------- link & chaining -----------------
		// load link, which is pre-computed in allocate_channel1()
		if (i == 0)
			paramSet.linkAddr = chinfo->link;
		else if (i == cnt - 1)
			paramSet.linkAddr = 0xffff;
		else
			paramSet.linkAddr = chinfo->links[i - 1];

		if (i == 0) { // only do this once
			/* Src & Dest are in INCR modes */
			paramSet.opt &= 0xFFFFFFFCu;
			/* Program the TCC -- since we are chaining,
			 * this: 1) chain to itself for xfers 0..n-2
			 * 2) set tcc irq code for xfer n-1 */
			paramSet.opt &= (~(OPT_TCC_MASK));
			paramSet.opt |= ((phys_chId << OPT_TCC_SHIFT) & OPT_TCC_MASK);
		}
		// in theory, the last PSET's tcc should be really @tcc instead of
		// phys chId. we assume...
		assert(tcc == phys_chId);

		if (i != cnt - 1) {
			/* no intermediate complete irq */
			paramSet.opt &= ~(1 << OPT_ITCINTEN_SHIFT);
			/* no complete irq */
			paramSet.opt &= ~(1 << OPT_TCINTEN_SHIFT);
			/* yes complete chaining */
			paramSet.opt |= (1 << OPT_TCCHEN_SHIFT);
			/* no intermediate complete chaining */
			paramSet.opt &= ~(1 << OPT_ITCCHEN_SHIFT);
		} else { // last PSET
			/* no intermediate complete irq */
			paramSet.opt &= ~(1 << OPT_ITCINTEN_SHIFT);
			/* yes complete irq */
			paramSet.opt |= (1 << OPT_TCINTEN_SHIFT);
			/* no complete chaining */
			paramSet.opt &= ~(1 << OPT_TCCHEN_SHIFT);
			/* no intermediate complete chaining */
			paramSet.opt &= ~(1 << OPT_ITCCHEN_SHIFT);
		}

		/* Now, write the PaRAM Set. */
		result = EDMA3_DRV_setPaRAM(hEdma, chid, &paramSet);
		assert(result == EDMA3_DRV_SOK);

		prev_size = size;
	}

	k2_measure("start_gather:psets");

	// debugging
	//	dump_chinfo(chinfo);

	xfer->chinfo = chinfo;
	xfer->cnt = cnt;
	xfer->totalsize = dest_off;
	//	gettimeofday(&(xfer->start), NULL);
	result = EDMA3_DRV_enableTransfer(hEdma, phys_chId,
	                                  EDMA3_DRV_TRIG_MODE_MANUAL);
	checkret();

	k2_measure("start_gather:done");

	return xfer;
}

/* The ``preloaded''-psets version.
 * gathers a bunch of source buffers into one dest buffer (contig).
 *
 * we count on that the geometry of all PSETs (phys/logic)
 * are loaded (and are restored to default if clobbered).
 *
 * @handle: channel handle, a pointer returned by
 * edma3_allocate_channel_linked()
 *
 * @srcs, @src_stride: specifying the source buffers.
 *
 * @srcs: the addr of the _virt_ pointer to the 1st source buffer,
 * 	the next pointer is @src_stride away
 *
 * @dest: the dma (phys) addr of the dest buffer
 *
 * the user should ensure @dest buffer has enough room.
 *
 * return: xfer handle */
struct edma_xfer_scatter *edma3_start_gather_preloaded(void *handle,
			        unsigned long *srcs, int src_stride,
			        unsigned long *voffsets, int voffset_stride,
			        unsigned long cnt, unsigned long dest)
{
	struct dma_ch_info *chinfo = handle;
	assert(handle);
	EDMA3_DRV_Handle hEdma = chinfo->hedma;
	unsigned int phys_chId = chinfo->ch;
	assert(chinfo->pset);	// a sign showing all psets are preloaded

	unsigned long dest_off = 0;  // offset in dest buffer
	unsigned long size = chinfo->size; // shoudld equal to xfer size

	EDMA3_DRV_Result result = EDMA3_DRV_SOK;

	/* sanity chk */
	assert(src_stride == 0 || src_stride >= sizeof(unsigned long *));
	assert(voffset_stride == 0 || voffset_stride >= sizeof(unsigned long *));

	/* the xfer desc. used to wait for xfer results */
	struct edma_xfer_scatter *xfer = \
				                                 (struct edma_xfer_scatter *)malloc(sizeof(struct edma_xfer_scatter));
	assert(xfer);

	assert(chinfo->logic_cnt + 1 >= cnt);  // we have enough PSETs preallocated

	// go through each src/size to populate PSETs
	// i is the number of xfer (PSET), including the phys one.
	unsigned long *psrc = srcs;   // pointer to the buffer pointer
	unsigned long *pvoffset = voffsets;
	unsigned long src, voffset; // src: buffer's dma addr

	k2_measure("start_gatherp:inited");

	for (int i = 0; i < cnt; i++)
{
		int chid = ((i == 0) ? phys_chId : chinfo->logic_chs[i - 1]);

		voffset = *pvoffset;
		assert(voffset > 0);
		src = *psrc - voffset;	// *psrc: the buffer virt addr. convert to dma addr

		// advance @psrc and @pvoffset
		psrc = (unsigned long *)((unsigned long) psrc + src_stride); // find the next buffer pointer
		if (voffset_stride != 0)
			pvoffset = (unsigned long *)((unsigned long) pvoffset + voffset_stride);

		// update src/dest
		result = EDMA3_DRV_setPaRAMEntry(hEdma, chid, EDMA3_DRV_PARAM_ENTRY_SRC,
		                                 src);
		checkret();
		// XXX this can be preloaded too
		result = EDMA3_DRV_setPaRAMEntry(hEdma, chid, EDMA3_DRV_PARAM_ENTRY_DST,
		                                 dest + dest_off);
		checkret();

		dest_off += size;

		// ------------- link & chaining -----------------
		// only has to fix the last link
		if (i == cnt - 1) {
			// xzl: clumsy API -- bad
			result = EDMA3_DRV_setPaRAMEntry(hEdma, chid, EDMA3_DRV_PARAM_ENTRY_LINK_BCNTRLD,
			                                 chinfo->last_brcnt_link);
			checkret();

			result = EDMA3_DRV_setPaRAMEntry(hEdma, chid, EDMA3_DRV_PARAM_ENTRY_OPT,
			                                 chinfo->last_opt);
			checkret();
		}
	}

	k2_measure("start_gatherp:psets");

	// debugging
	//	I("%s:dumping chinfo...", __func__);
	//	dump_chinfo(chinfo);

	xfer->chinfo = chinfo;
	xfer->cnt = cnt;
	xfer->totalsize = dest_off;
	//	gettimeofday(&(xfer->start), NULL);
	result = EDMA3_DRV_enableTransfer(hEdma, phys_chId,
	                                  EDMA3_DRV_TRIG_MODE_MANUAL);
	checkret();

	k2_measure("start_gather:done");

	return xfer;
}

// chain the channel to itself. requires no cpu involvement.
// split @totalsize among @count xfers
int edma3_start_xfer_multiple_selfchaining(EDMA3_DRV_Handle hEdma, unsigned long src,
        unsigned long dest, unsigned long totalsize, int count, void **xfer)
{
	EDMA3_DRV_Result result = EDMA3_DRV_SOK;
	EDMA3_DRV_PaRAMRegs paramSet = {0,0,0,0,0,0,0,0,0,0,0,0};
	unsigned int chId = 0;
	unsigned int * logic_chs = NULL;  // an array of logic channels
	unsigned int prev_ch; //xzl: used in linking channels.
	unsigned int tcc = 0;
	int i;
	unsigned long size = totalsize / count; // per xfer
	//	unsigned long size = totalsize;
	assert(totalsize % count == 0);
	unsigned int BRCnt = 0;
	int acnt = 0, bcnt = 0, ccnt = 1;
	int srcbidx = 0, desbidx = 0;
	int srccidx = 0, descidx = 0;
	int max_narrays = SZ_32K;
	int syncType;
	const char *syncdesc;

	/* the xfer desc. used to wait for xfer results */
	*xfer = (struct edma_xfer *)malloc(sizeof(struct edma_xfer));
	assert(*xfer);

	/* setting the xfer size. use AB sync that can xfer up to 4096M.
	 *
	 * the cpu should trigger (''sync'') once for the entire transfer.
	 * thus, we choose A sync or AB sync properly.
	 * XXX: we didn't check size > 1 frame (4gig)
	 * */

	/* 64k if single array, otherwise 32k (bounded by bidx) */
#define MAX_ARRAY_SIZE_SINGLE  	SZ_64K
	#define MAX_ARRAY_SIZE_MULTI	SZ_32K

	if (size < MAX_ARRAY_SIZE_SINGLE) {	/* can fit in one array */
		syncType = EDMA3_DRV_SYNC_A;
		syncdesc = "Sync A";
		acnt = size;
		bcnt = 1;
	} else { /* can't fit in one array. how many arrays do we need? */
		int narrays = 1;
		syncType = EDMA3_DRV_SYNC_AB;
		syncdesc = "Sync AB";
		bcnt = acnt = -1;
		while (narrays <= max_narrays) {
			narrays *= 2;
			if (size % narrays != 0 || size / narrays >= MAX_ARRAY_SIZE_MULTI)
				continue;
			bcnt = narrays;
			acnt = size / narrays;
			break;
		}

		if (bcnt == -1) {
			fprintf(stderr, "%s:not impl: can't evenly distribute %lu bytes into arrays.\n",
			        __func__, size);
			assert(0);
			return EDMA3_DRV_E_INVALID_PARAM;
		}
	}

	/* Set B count reload as B count. */
	BRCnt = bcnt;	/* unused for AB sync */
	srcbidx = (int) acnt;
	desbidx = (int) acnt;

	if (syncType == EDMA3_DRV_SYNC_A) {
		/* A Sync Transfer Mode */
		srccidx = (int) acnt;
		descidx = (int) acnt;
	} else {
		/* AB Sync Transfer Mode */
		srccidx = ((int) acnt * (int) bcnt);
		descidx = ((int) acnt * (int) bcnt);
	}

	/* sanity check indices. when ccnt==1, the xfer is done in one burst,
	 * making cidx irrelevant. same for bcnt. */
	if (bcnt > 1)
		assert(srcbidx <= 32767 && desbidx <= 32767);
	if (ccnt > 1)
		assert(srccidx <= 32767 && descidx <= 32767);

	D("xzl:%s: mode: %s acnt %d bcnt %d ccnt %d\n", __func__,
	  syncdesc, acnt, bcnt, ccnt);

	/* Setup for channel 1 (the physical channel, or called "master" channel in some example) */
	tcc = EDMA3_DRV_TCC_ANY;
	//	chId = EDMA3_DRV_DMA_CHANNEL_ANY;	// request a physical dma channel
	chId = 2;	// easier for debugging?

	assert(result == EDMA3_DRV_SOK);

	result = EDMA3_DRV_requestChannel(hEdma, &chId, &tcc,
	                                  (EDMA3_RM_EventQueue) 0, &callback1, *xfer);
	assert(result == EDMA3_DRV_SOK);

	I("obtained channel: chid %d (tcc %d)", chId, tcc);

	/* --------------------------------------------------
	 * Fill the PaRAM Set with transfer specific information
	 * --------------------------------------------------*/
	paramSet.srcAddr = src;
	paramSet.destAddr = dest;

	/**
	 * Be Careful !!!
	 * Valid values for SRCBIDX/DSTBIDX are between .32768 and 32767
	 * Valid values for SRCCIDX/DSTCIDX are between .32768 and 32767
	 */
	paramSet.srcBIdx = srcbidx;
	paramSet.destBIdx = desbidx;
	paramSet.srcCIdx = srccidx;
	paramSet.destCIdx = descidx;

	/**
	 * Be Careful !!!
	 * Valid values for ACNT/BCNT/CCNT are between 0 and 65535.
	 * ACNT/BCNT/CCNT must be greater than or equal to 1.
	 * Maximum number of bytes in an array (ACNT) is 65535 bytes
	 * Maximum number of arrays in a frame (BCNT) is 65535
	 * Maximum number of frames in a block (CCNT) is 65535
	 */
	paramSet.aCnt = acnt;
	paramSet.bCnt = bcnt;
	paramSet.cCnt = ccnt;

	/* For AB-synchronized transfers, BCNTRLD is not used. */
	paramSet.bCntReload = BRCnt;

	paramSet.linkAddr = 0xFFFFu;

	/* Src & Dest are in INCR modes */
	paramSet.opt &= 0xFFFFFFFCu;
	/* Program the TCC -- since we are chaining, this chain to itself */
	paramSet.opt |= ((tcc << OPT_TCC_SHIFT) & OPT_TCC_MASK);

	/* no intermediate complete irq */
	paramSet.opt &= ~(1 << OPT_ITCINTEN_SHIFT);
	/* no complete irq */
	paramSet.opt &= ~(1 << OPT_TCINTEN_SHIFT);
	/* yes complete chaining */
	paramSet.opt |= (1 << OPT_TCCHEN_SHIFT);
	/* no intermediate complete chaining */
	paramSet.opt &= ~(1 << OPT_ITCCHEN_SHIFT);

	if (syncType == EDMA3_DRV_SYNC_A) {
		paramSet.opt &= 0xFFFFFFFBu;
	} else {
		/* AB Sync Transfer Mode */
		paramSet.opt |= (1 << OPT_SYNCDIM_SHIFT);
	}

	/* Now, write the PaRAM Set. */
	result = EDMA3_DRV_setPaRAM(hEdma, chId, &paramSet);
	assert(result == EDMA3_DRV_SOK);

	/* ------------------------------------
	 * Grab (N-1) param sets and link them ("logic channels" in LLD's lingo)
	 *
	 * Note: Don't use EDMA3_DRV_linkChannel() or EDMA3_DRV_getPaRAMPhyAddr(),
	 * which mistakely treat the virt addr as phys addr.
	 *
	 * tcc: only one tcc is needed for all xfers.
	 *
	 * triggering: since this is linking, the CPU needs to trigger EDMA3 after
	 * every PSET is completed.
	 * ----------------------------------- */
	unsigned short param_id, prev_param_id;
	unsigned int phys_addr;

	phys_addr = edma3_get_param_phys(hEdma, chId);
	D("edma phys %08x", phys_addr);
	param_id = (unsigned short)(phys_addr & 0xffff);
	prev_param_id =  param_id;
	D("*** ch %d param %08x", chId, param_id);

	logic_chs = malloc(sizeof(int) * (count - 1));
	assert(logic_chs);

	prev_ch = chId;

	for (i = 0; i < count-1; i++) {
		logic_chs[i] = EDMA3_DRV_LINK_CHANNEL;

		result = EDMA3_DRV_requestChannel(hEdma, &logic_chs[i], &tcc,
		                                  (EDMA3_RM_EventQueue) 0, &callback1, *xfer);
		assert(result == EDMA3_DRV_SOK);

		/* reuse the param set from the previous phys/logic channel. */

		// load previous param set, this internally just does memcpy
		result = EDMA3_DRV_getPaRAM(hEdma, prev_ch, &paramSet);
		assert(result == EDMA3_DRV_SOK);

		// set up linking
		phys_addr = edma3_get_param_phys(hEdma, logic_chs[i]);
		D("edma phys %08x", phys_addr);
		param_id = (unsigned short)(phys_addr & 0xffff);

		// set: prev->linkAddr = param_id
		paramSet.linkAddr = param_id;
		I("----> after linking, param %08x, linkAddr %08x", prev_param_id,
		  paramSet.linkAddr);
		// (re-)write the previous PSET
		result = EDMA3_DRV_setPaRAM(hEdma, prev_ch, &paramSet);
		assert(result == EDMA3_DRV_SOK);

		// program the current PSET, leaving link to NULL
		// src & dest changed; size unchanged
		paramSet.srcAddr = src + (i + 1) * size;
		paramSet.destAddr = dest + (i + 1) * size;
		paramSet.linkAddr = 0xffff;

		// set up chaining by programming PSET.Opt
		if (i != count - 2) {
			//			result = EDMA3_DRV_setOptField (hEdma, logic_chs[i], EDMA3_DRV_OPT_FIELD_TCC, chId);
			//			assert(result == EDMA3_DRV_SOK);
			//			result = EDMA3_DRV_setOptField (hEdma, logic_chs[i], EDMA3_DRV_OPT_FIELD_TCCHEN, 1u);
			//			assert(result == EDMA3_DRV_SOK);
			//			result = EDMA3_DRV_setOptField (hEdma, logic_chs[i], EDMA3_DRV_OPT_FIELD_TCINTEN, 0u);
			//			assert(result == EDMA3_DRV_SOK);
			//			result = EDMA3_DRV_setOptField (hEdma, logic_chs[i], EDMA3_DRV_OPT_FIELD_STATIC, 0u);
			//			assert(result == EDMA3_DRV_SOK);

			/* chain back to the phys dma channel */
			paramSet.opt |= ((chId << OPT_TCC_SHIFT) & OPT_TCC_MASK);
			/* no intermediate complete irq */
			paramSet.opt &= ~(1 << OPT_ITCINTEN_SHIFT);
			/* no complete irq */
			paramSet.opt &= ~(1 << OPT_TCINTEN_SHIFT);
			/* yes complete chaining */
			paramSet.opt |= (1 << OPT_TCCHEN_SHIFT);
			/* no intermediate complete chaining */
			paramSet.opt &= ~(1 << OPT_ITCCHEN_SHIFT);
		} else {	// last one
			//			result = EDMA3_DRV_setOptField (hEdma, logic_chs[i], EDMA3_DRV_OPT_FIELD_TCC, chId);
			//			assert(result == EDMA3_DRV_SOK);
			//			result = EDMA3_DRV_setOptField (hEdma, logic_chs[i], EDMA3_DRV_OPT_FIELD_TCCHEN, 0u);
			//			assert(result == EDMA3_DRV_SOK);
			//			result = EDMA3_DRV_setOptField (hEdma, logic_chs[i], EDMA3_DRV_OPT_FIELD_TCINTEN, 1u);
			//			assert(result == EDMA3_DRV_SOK);
			//			result = EDMA3_DRV_setOptField (hEdma, logic_chs[i], EDMA3_DRV_OPT_FIELD_STATIC, 0u);
			//			assert(result == EDMA3_DRV_SOK);

			/* tcc code as the phys dma channel */
			paramSet.opt |= ((chId << OPT_TCC_SHIFT) & OPT_TCC_MASK);
			/* no intermediate complete irq */
			paramSet.opt &= ~(1 << OPT_ITCINTEN_SHIFT);
			/* yes complete irq */
			paramSet.opt |= (1 << OPT_TCINTEN_SHIFT);
			/* no complete chaining */
			paramSet.opt &= ~(1 << OPT_TCCHEN_SHIFT);
			/* no intermediate complete chaining */
			paramSet.opt &= ~(1 << OPT_ITCCHEN_SHIFT);
		}

		result = EDMA3_DRV_setPaRAM(hEdma, logic_chs[i], &paramSet);
		assert(result == EDMA3_DRV_SOK);

		prev_ch = logic_chs[i];
		prev_param_id = param_id;
	}

	/* ------------------------------------
	 * Kick channel 1 (the phys one) to start xfer
	 * ----------------------------------- */
	int kickcnt = 0;
	I("kick %d", kickcnt++);
	result = EDMA3_DRV_enableTransfer(hEdma, chId,
	                                  EDMA3_DRV_TRIG_MODE_MANUAL);
	assert(result == EDMA3_DRV_SOK);

	I("start to wait...tcc %d", tcc);
	result = EDMA3_DRV_waitAndClearTcc(hEdma, tcc);	 // xzl: should use tcc (not chId)
	assert(result == EDMA3_DRV_SOK);
	I("wait done...");

	// after the last one, a zero'd PSET will be loaded
	phys_addr = edma3_get_param_phys(hEdma, chId);
	param_id = (unsigned short)(phys_addr & 0xffff);
	paramSet.linkAddr = 0;  // clean it to make sure we're loading value
	result = EDMA3_DRV_getPaRAM(hEdma, chId, &paramSet);
	I("PSET %08x linkAddr %08x TCC %d", param_id, paramSet.linkAddr,
	  (paramSet.opt >> 12) & ((1<<6) - 1));

	I("all %d xfer done\n", i+1);
	//	sleep(1); // this works

	// cleanup
	result = EDMA3_DRV_freeChannel(hEdma, chId);
	assert(result == EDMA3_DRV_SOK);

	for (i = 0; i < count-1; i++) {
		result = EDMA3_DRV_freeChannel(hEdma, logic_chs[i]);
		assert(result == EDMA3_DRV_SOK);
	}

	return 0;
}

static void dump_chinfo(struct dma_ch_info *chinfo)
{
	EDMA3_DRV_Handle hEdma = chinfo->hedma;
	unsigned int chId = chinfo->ch;
	EDMA3_DRV_Result result;
	EDMA3_DRV_PaRAMRegs pset;

	// debugging: dump all ch info
	result = EDMA3_DRV_getPaRAM(hEdma, chId, &pset);
	checkret();

	I("dumping channels --- ");
	printf("type ch pset sv.ln link src dest acnt bcnt ccnt srcBIdx destBIdx srcCIdx destCIdx\n");
	printf("phys %2u %04lx %04x %04x %08x %08x %5u %5u %5u %5u %5u %5u %5u %08x\n", chId,
	       edma3_get_param_phys(hEdma, chId) & 0xffff,
	       chinfo->link, pset.linkAddr,
	       pset.srcAddr, pset.destAddr, pset.aCnt, pset.bCnt, pset.cCnt,
	       pset.srcBIdx, pset.destBIdx, pset.srcCIdx, pset.destCIdx,
	       pset.opt);

	for (int i = 0; i < chinfo->logic_cnt; i++)
	{
		result = EDMA3_DRV_getPaRAM(hEdma, chinfo->logic_chs[i], &pset);
		checkret();

		// dump the stored links and the links from PSET side-by-side.
		printf("logi %2u %04lx %04x %04x %08x %08x %5u %5u %5u %5u %5u %5u %5u %08x\n",
		       chinfo->logic_chs[i],
		       edma3_get_param_phys(hEdma, chinfo->logic_chs[i]) & 0xffff,
		       chinfo->links[i], pset.linkAddr,
		       pset.srcAddr, pset.destAddr, pset.aCnt, pset.bCnt, pset.cCnt,
		       pset.srcBIdx, pset.destBIdx, pset.srcCIdx, pset.destCIdx,
		       pset.opt);
	}
	printf("tcc %x \n", chinfo->tcc);
	printf("--------------\n");
}

/* allocate one phys cghannel and @logic_cnt logic channels (i.e. PSETs)
 *
 * return: an opaque handle (which is actually pointer to a @dma_ch_info */
struct dma_ch_info *edma3_allocate_channel_linked(EDMA3_DRV_Handle hEdma, int logic_cnt)
{
	struct dma_ch_info *chinfo;
	unsigned int tcc = EDMA3_DRV_TCC_ANY;
	unsigned int chId = EDMA3_DRV_DMA_CHANNEL_ANY; // any (physical) channel would do
	EDMA3_DRV_Result result;

	chinfo = malloc(sizeof(struct dma_ch_info));
	assert(chinfo);

	result = EDMA3_DRV_requestChannel(hEdma, &chId, &tcc,
	                                  (EDMA3_RM_EventQueue) 0, NULL, NULL);
	assert(result == EDMA3_DRV_SOK);

	chinfo->tcc = tcc;
	chinfo->ch = chId;
	chinfo->hedma = hEdma;
	chinfo->pset = NULL;

	// safeguard?
	assert(tcc != EDMA3_DRV_TCC_ANY);
	assert(chId != EDMA3_DRV_DMA_CHANNEL_ANY);

	// pre allocate logic channels (PSETs)
	chinfo->logic_cnt = logic_cnt;
	chinfo->logic_chs = malloc(logic_cnt * sizeof(unsigned int));
	chinfo->links = malloc(logic_cnt * sizeof(unsigned short));

	assert(chinfo->logic_chs);

	tcc = EDMA3_DRV_TCC_ANY;
	unsigned short param_id;
	for (int i = 0; i < logic_cnt; i++)
	{
		chinfo->logic_chs[i] = EDMA3_DRV_LINK_CHANNEL;
		result = EDMA3_DRV_requestChannel(hEdma, &(chinfo->logic_chs[i]), &tcc,
		                                  (EDMA3_RM_EventQueue) 0, NULL, NULL);
		checkret();
		param_id = (edma3_get_param_phys(hEdma, chinfo->logic_chs[i]) & 0xffff);
		D("allocate logic ch %d paramid %x", chinfo->logic_chs[i], param_id);

		if (i == 0)
			chinfo->link = param_id;
		else
			chinfo->links[i - 1] = param_id;
	}

	chinfo->links[logic_cnt - 1] = 0xffff;

	//	dump_chinfo(chinfo);
	return chinfo;
}

/* return: an opaque handle (which is actually pointer to a @dma_ch_info */
struct dma_ch_info *edma3_allocate_channel(EDMA3_DRV_Handle hEdma)
{
	struct dma_ch_info *chinfo;
	unsigned int tcc = EDMA3_DRV_TCC_ANY;
	unsigned int chId = EDMA3_DRV_DMA_CHANNEL_ANY; // any (physical) channel would do
	EDMA3_DRV_Result result;

	chinfo = malloc(sizeof(struct dma_ch_info));
	assert(chinfo);

	result = EDMA3_DRV_requestChannel(hEdma, &chId, &tcc,
	                                  (EDMA3_RM_EventQueue) 0, NULL, NULL);
	assert(result == EDMA3_DRV_SOK);

	// safeguard?
	assert(tcc != EDMA3_DRV_TCC_ANY);
	assert(chId != EDMA3_DRV_DMA_CHANNEL_ANY);

	chinfo->tcc = tcc;
	chinfo->ch = chId;
	chinfo->hedma = hEdma;
	chinfo->logic_cnt = 0;
	chinfo->logic_chs = NULL;
	chinfo->pset = NULL;

	return chinfo;
}

void edma3_free_channel(struct dma_ch_info *h)
{
	struct dma_ch_info *chinfo = h;
	EDMA3_DRV_Result ret;
	assert(h);

	ret = EDMA3_DRV_freeChannel(chinfo->hedma, chinfo->ch);
	assert(ret == EDMA3_DRV_SOK);

	for (int i = 0; i < chinfo->logic_cnt; i++)
	{
		ret = EDMA3_DRV_freeChannel(chinfo->hedma, chinfo->logic_chs[i]);
		assert(ret == EDMA3_DRV_SOK);
	}

	if (chinfo->logic_chs)
		free(chinfo->logic_chs);
	if (chinfo->links)
		free(chinfo->links);
	if (chinfo->pset)
		free(chinfo->pset);
	free(chinfo);
}

/* @handle -- the one created by edma3_allocate_channel()
 * @src, dest -- both phys
 * @size -- in bytes. max 4G (one edma3 frame)
 * @cc -- 1: take care of cache coherence
 */
EDMA3_DRV_Result edma3_start_xfer_once1 (void *handle, unsigned long src,
        unsigned long dest, unsigned long size, struct edma_xfer **xfer)
{
	struct dma_ch_info *chinfo = handle;
	assert(handle);
	EDMA3_DRV_Handle hEdma = chinfo->hedma;
	unsigned int chId = chinfo->ch;
	unsigned int tcc = chinfo->tcc;

	EDMA3_DRV_Result result = EDMA3_DRV_SOK;
	EDMA3_DRV_PaRAMRegs paramSet = {0,0,0,0,0,0,0,0,0,0,0,0};
	//	int i;
	//	unsigned int numenabled = 0;
	unsigned int BRCnt = 0;
	int acnt = 0, bcnt = 0, ccnt = 1;
	int srcbidx = 0, desbidx = 0;
	int srccidx = 0, descidx = 0;
	int max_narrays = SZ_32K;

	//	char *src_virt, *dest_virt;  	/* need them to flush/inv cache & verify results */

	int syncType;
	const char *syncdesc;

	/* the xfer desc. used to wait for xfer results */
	*xfer = (struct edma_xfer *)malloc(sizeof(struct edma_xfer));
	assert(*xfer);
	struct edma_xfer *exfer = (struct edma_xfer *)(*xfer); // handy

	/* setting the xfer size. use AB sync that can xfer up to 4096M.
	 *
	 * the cpu should trigger (''sync'') once for the entire transfer.
	 * thus, we choose A sync or AB sync properly.
	 * XXX: we didn't check size > 1 frame (4gig)
	 * */

	/* 64k if single array, otherwise 32k (bounded by bidx) */
#define MAX_ARRAY_SIZE_SINGLE  	SZ_64K
#define MAX_ARRAY_SIZE_MULTI	SZ_32K

	if (size < MAX_ARRAY_SIZE_SINGLE)
	{	/* can fit in one array */
		syncType = EDMA3_DRV_SYNC_A;
		syncdesc = "Sync A";
		acnt = size;
		bcnt = 1;
	} else
	{ /* can't fit in one array. how many arrays do we need? */
		int narrays = 1;
		syncType = EDMA3_DRV_SYNC_AB;
		syncdesc = "Sync AB";
		bcnt = acnt = -1;
		while (narrays <= max_narrays) {
			narrays *= 2;
			if (size % narrays != 0 || size / narrays >= MAX_ARRAY_SIZE_MULTI)
				continue;
			bcnt = narrays;
			acnt = size / narrays;
			break;
		}

		if (bcnt == -1) {
			fprintf(stderr, "%s:not impl: can't evenly distribute %lu bytes into arrays.\n",
			        __func__, size);
			assert(0);
			return EDMA3_DRV_E_INVALID_PARAM;
		}

#if 0
		// if swap the following two, the first 3 words in dest buffer are
		// corrupted. why?
		bcnt = size / SZ_32K;
		acnt = SZ_32K;
#endif

	}

	/* Set B count reload as B count. */
	BRCnt = bcnt;	/* unused for AB sync */
	srcbidx = (int) acnt;
	desbidx = (int) acnt;

	if (syncType == EDMA3_DRV_SYNC_A)
	{
		/* A Sync Transfer Mode */
		srccidx = (int) acnt;
		descidx = (int) acnt;
	} else
	{
		/* AB Sync Transfer Mode */
		srccidx = ((int) acnt * (int) bcnt);
		descidx = ((int) acnt * (int) bcnt);
	}

	/* sanity check indices. when ccnt==1, the xfer is done in one burst,
	 * making cidx irrelevant. same for bcnt. */
	if (bcnt > 1)
		assert(srcbidx <= 32767 && desbidx <= 32767);
	if (ccnt > 1)
		assert(srccidx <= 32767 && descidx <= 32767);

#if 0 /* no need for cc if using a normal TI keystone kernel, which turns on dma coherence*/

	if (cc)
	{
		src_virt = (char*) edma3_mmap(src, size);
		dest_virt = (char*) edma3_mmap(dest, size);

		assert(src_virt);
		assert(dest_virt);

		/* Flush the Source Buffer */
		if (result == EDMA3_DRV_SOK) {
			result = Edma3_CacheFlush((unsigned int)src_virt, size);
		}

		/* Invalidate the Destination Buffer */
		if (result == EDMA3_DRV_SOK) {
			result = Edma3_CacheInvalidate((unsigned int)dest_virt, size);
		}

		/* XXX unmap? */
		assert(0);
	}
#endif

	D("xzl:%s: mode: %s acnt %d bcnt %d ccnt %d\n", __func__,
	  syncdesc, acnt, bcnt, ccnt);

	/* Fill the PaRAM Set with transfer specific information */
	if (result == EDMA3_DRV_SOK)
	{

		paramSet.srcAddr = src;
		paramSet.destAddr = dest;

		/**
		 * Be Careful !!!
		 * Valid values for SRCBIDX/DSTBIDX are between .32768 and 32767
		 * Valid values for SRCCIDX/DSTCIDX are between .32768 and 32767
		 */
		paramSet.srcBIdx = srcbidx;
		paramSet.destBIdx = desbidx;
		paramSet.srcCIdx = srccidx;
		paramSet.destCIdx = descidx;

		/**
		 * Be Careful !!!
		 * Valid values for ACNT/BCNT/CCNT are between 0 and 65535.
		 * ACNT/BCNT/CCNT must be greater than or equal to 1.
		 * Maximum number of bytes in an array (ACNT) is 65535 bytes
		 * Maximum number of arrays in a frame (BCNT) is 65535
		 * Maximum number of frames in a block (CCNT) is 65535
		 */
		paramSet.aCnt = acnt;
		paramSet.bCnt = bcnt;
		paramSet.cCnt = ccnt;

		/* For AB-synchronized transfers, BCNTRLD is not used. */
		paramSet.bCntReload = BRCnt;

		paramSet.linkAddr = 0xFFFFu;

		/* Src & Dest are in INCR modes */
		paramSet.opt &= 0xFFFFFFFCu;
		/* Program the TCC */
		paramSet.opt |= ((tcc << OPT_TCC_SHIFT) & OPT_TCC_MASK);

		/* Enable Intermediate & Final transfer completion interrupt.
		 * XXX: turn off intermediate irq? */
		paramSet.opt |= (1 << OPT_ITCINTEN_SHIFT);
		//paramSet.opt &= ~(1 << OPT_ITCINTEN_SHIFT);
		paramSet.opt |= (1 << OPT_TCINTEN_SHIFT);

		if (syncType == EDMA3_DRV_SYNC_A) {
			paramSet.opt &= 0xFFFFFFFBu;
		} else {
			/* AB Sync Transfer Mode */
			paramSet.opt |= (1 << OPT_SYNCDIM_SHIFT);
		}

		/* Now, write the PaRAM Set. */
		result = EDMA3_DRV_setPaRAM(hEdma, chId, &paramSet);
		assert(result == EDMA3_DRV_SOK);
	}

	/*
	 * Since the transfer is going to happen in Manual mode of EDMA3
	 * operation, we have to 'Enable the Transfer' multiple times.
	 * Number of times depends upon the Mode (A/AB Sync)
	 * and the different counts.
	 *
	 * xzl: will only irq once (AB sync, with ccnt = 1)
	 */
	if (result == EDMA3_DRV_SOK)
	{

		//		gettimeofday(&(exfer->start), NULL);

		result = EDMA3_DRV_enableTransfer(hEdma, chId,
		                                  EDMA3_DRV_TRIG_MODE_MANUAL);

		if (result != EDMA3_DRV_SOK) {
			fprintf(stderr, "edma3_test: EDMA3_DRV_enableTransfer "
			        "Failed, error code: %d\n", result);
			goto err;
		}

		// fill in the desc
		exfer->chId = chId;
		exfer->tcc = tcc;
		exfer->hEdma = hEdma;

		return EDMA3_DRV_SOK;
	}

err:
	fprintf(stderr, "bug: failed to start the xfer. why?\n");
	assert(0);
	return EDMA3_DRV_E_INVALID_PARAM; /* XXX better code */
}

/*
 * @src, dest -- both phys
 * @size -- in bytes. max 4G (one edma3 frame)
 * @cc -- 1: take care of cache coherence
 */
EDMA3_DRV_Result edma3_start_xfer_once(EDMA3_DRV_Handle hEdma, unsigned long src,
                                       unsigned long dest, unsigned long size, int cc, void **xfer)
{

	EDMA3_DRV_Result result = EDMA3_DRV_SOK;
	EDMA3_DRV_PaRAMRegs paramSet = {0,0,0,0,0,0,0,0,0,0,0,0};
	unsigned int chId = 0;
	unsigned int tcc = 0;
	//	int i;
	//	unsigned int numenabled = 0;
	unsigned int BRCnt = 0;
	int acnt = 0, bcnt = 0, ccnt = 1;
	int srcbidx = 0, desbidx = 0;
	int srccidx = 0, descidx = 0;
	int max_narrays = SZ_32K;

	//	char *src_virt, *dest_virt;  	/* need them to flush/inv cache & verify results */

	int syncType;
	const char *syncdesc;

	/* the xfer desc. used to wait for xfer results */
	*xfer = (struct edma_xfer *)malloc(sizeof(struct edma_xfer));
	assert(*xfer);
	struct edma_xfer *exfer = (struct edma_xfer *)(*xfer); // handy

	/* setting the xfer size. use AB sync that can xfer up to 4096M.
	 *
	 * the cpu should trigger (''sync'') once for the entire transfer.
	 * thus, we choose A sync or AB sync properly.
	 * XXX: we didn't check size > 1 frame (4gig)
	 * */

	/* 64k if single array, otherwise 32k (bounded by bidx) */
#define MAX_ARRAY_SIZE_SINGLE  	SZ_64K
#define MAX_ARRAY_SIZE_MULTI	SZ_32K

	if (size < MAX_ARRAY_SIZE_SINGLE) {	/* can fit in one array */
		syncType = EDMA3_DRV_SYNC_A;
		syncdesc = "Sync A";
		acnt = size;
		bcnt = 1;
	} else { /* can't fit in one array. how many arrays do we need? */
		int narrays = 1;
		syncType = EDMA3_DRV_SYNC_AB;
		syncdesc = "Sync AB";
		bcnt = acnt = -1;
		while (narrays <= max_narrays) {
			narrays *= 2;
			if (size % narrays != 0 || size / narrays >= MAX_ARRAY_SIZE_MULTI)
				continue;
			bcnt = narrays;
			acnt = size / narrays;
			break;
		}

		if (bcnt == -1) {
			fprintf(stderr, "%s:not impl: can't evenly distribute %lu bytes into arrays.\n",
			        __func__, size);
			assert(0);
			return EDMA3_DRV_E_INVALID_PARAM;
		}

#if 0
		// if swap the following two, the first 3 words in dest buffer are
		// corrupted. why?
		bcnt = size / SZ_32K;
		acnt = SZ_32K;
#endif

	}

	/* Set B count reload as B count. */
	BRCnt = bcnt;	/* unused for AB sync */
	srcbidx = (int) acnt;
	desbidx = (int) acnt;

	if (syncType == EDMA3_DRV_SYNC_A) {
		/* A Sync Transfer Mode */
		srccidx = (int) acnt;
		descidx = (int) acnt;
	} else {
		/* AB Sync Transfer Mode */
		srccidx = ((int) acnt * (int) bcnt);
		descidx = ((int) acnt * (int) bcnt);
	}

	/* sanity check indices. when ccnt==1, the xfer is done in one burst,
	 * making cidx irrelevant. same for bcnt. */
	if (bcnt > 1)
		assert(srcbidx <= 32767 && desbidx <= 32767);
	if (ccnt > 1)
		assert(srccidx <= 32767 && descidx <= 32767);

#if 0 /* no need for cc if using a normal TI keystone kernel, which turns on dma coherence*/

	if (cc) {
		src_virt = (char*) edma3_mmap(src, size);
		dest_virt = (char*) edma3_mmap(dest, size);

		assert(src_virt);
		assert(dest_virt);

		/* Flush the Source Buffer */
		if (result == EDMA3_DRV_SOK) {
			result = Edma3_CacheFlush((unsigned int)src_virt, size);
		}

		/* Invalidate the Destination Buffer */
		if (result == EDMA3_DRV_SOK) {
			result = Edma3_CacheInvalidate((unsigned int)dest_virt, size);
		}

		/* XXX unmap? */
		assert(0);
	}
#endif

	D("xzl:%s: mode: %s acnt %d bcnt %d ccnt %d\n", __func__,
	  syncdesc, acnt, bcnt, ccnt);
	/* Setup for Channel 1*/
	tcc = EDMA3_DRV_TCC_ANY;		/* tcc: xfer complete code. mapping to ISR? */
	chId = EDMA3_DRV_DMA_CHANNEL_ANY;

	/* Request any DMA channel and any TCC */
	assert(result == EDMA3_DRV_SOK);
	if (result == EDMA3_DRV_SOK) {
		// xzl: this dma channel uses eventqueue 0, which is (by default) mapped
		// to tc0.
		result = EDMA3_DRV_requestChannel(hEdma, &chId, &tcc,
		                                  (EDMA3_RM_EventQueue) 0,
		                                  // cb func. userspace driver uses polling. callback unused.
		                                  NULL,
		                                  // &callback1,
		                                  NULL);
		if (result != EDMA3_DRV_SOK) {
			W("failed. error code %d (base%d)", result, result - EDMA3_DRV_E_BASE);
			assert(0);
		}
	}

	/* Fill the PaRAM Set with transfer specific information */
	if (result == EDMA3_DRV_SOK) {

		paramSet.srcAddr = src;
		paramSet.destAddr = dest;

		/**
		 * Be Careful !!!
		 * Valid values for SRCBIDX/DSTBIDX are between .32768 and 32767
		 * Valid values for SRCCIDX/DSTCIDX are between .32768 and 32767
		 */
		paramSet.srcBIdx = srcbidx;
		paramSet.destBIdx = desbidx;
		paramSet.srcCIdx = srccidx;
		paramSet.destCIdx = descidx;

		/**
		 * Be Careful !!!
		 * Valid values for ACNT/BCNT/CCNT are between 0 and 65535.
		 * ACNT/BCNT/CCNT must be greater than or equal to 1.
		 * Maximum number of bytes in an array (ACNT) is 65535 bytes
		 * Maximum number of arrays in a frame (BCNT) is 65535
		 * Maximum number of frames in a block (CCNT) is 65535
		 */
		paramSet.aCnt = acnt;
		paramSet.bCnt = bcnt;
		paramSet.cCnt = ccnt;

		/* For AB-synchronized transfers, BCNTRLD is not used. */
		paramSet.bCntReload = BRCnt;

		paramSet.linkAddr = 0xFFFFu;

		/* Src & Dest are in INCR modes */
		paramSet.opt &= 0xFFFFFFFCu;
		/* Program the TCC */
		paramSet.opt |= ((tcc << OPT_TCC_SHIFT) & OPT_TCC_MASK);

		/* Enable Intermediate & Final transfer completion interrupt.
		 * XXX: turn off intermediate irq? */
		paramSet.opt |= (1 << OPT_ITCINTEN_SHIFT);
		//paramSet.opt &= ~(1 << OPT_ITCINTEN_SHIFT);
		paramSet.opt |= (1 << OPT_TCINTEN_SHIFT);

		if (syncType == EDMA3_DRV_SYNC_A) {
			paramSet.opt &= 0xFFFFFFFBu;
		} else {
			/* AB Sync Transfer Mode */
			paramSet.opt |= (1 << OPT_SYNCDIM_SHIFT);
		}

		/* Now, write the PaRAM Set. */
		result = EDMA3_DRV_setPaRAM(hEdma, chId, &paramSet);
		assert(result == EDMA3_DRV_SOK);
	}

	/*
	 * Since the transfer is going to happen in Manual mode of EDMA3
	 * operation, we have to 'Enable the Transfer' multiple times.
	 * Number of times depends upon the Mode (A/AB Sync)
	 * and the different counts.
	 *
	 * xzl: will only irq once (AB sync, with ccnt = 1)
	 */
	if (result == EDMA3_DRV_SOK) {

		//		gettimeofday(&(exfer->start), NULL);

		result = EDMA3_DRV_enableTransfer(hEdma, chId,
		                                  EDMA3_DRV_TRIG_MODE_MANUAL);

		if (result != EDMA3_DRV_SOK) {
			fprintf(stderr, "edma3_test: EDMA3_DRV_enableTransfer "
			        "Failed, error code: %d\n", result);
			goto err;
		}

		// fill in the desc
		exfer->chId = chId;
		exfer->tcc = tcc;
		exfer->hEdma = hEdma;

		return EDMA3_DRV_SOK;
	}

err:
	fprintf(stderr, "bug: failed to start the xfer. why?\n");
	assert(0);
	return EDMA3_DRV_E_INVALID_PARAM; /* XXX better code */
}

EDMA3_DRV_Result edma3_wait_xfer(void *xfer)
{
	EDMA3_DRV_Result ret;
	//	struct timeval end;
	struct edma_xfer *exfer = (struct edma_xfer *)(xfer); // handy
	assert(exfer);

	// does the following func wait on intermediate irq? :(
	ret = EDMA3_DRV_waitAndClearTcc(exfer->hEdma, exfer->tcc); //should not use chId
	//	gettimeofday(&end, NULL);

	//	D("Transfer completed! %ld us", __func__,
	//	  (end.tv_sec * 1000000 + end.tv_usec)
	//	  - (exfer->start.tv_sec * 1000000 + exfer->start.tv_usec));

	assert(ret == EDMA3_DRV_SOK);

	/* Free the previously allocated channel.
	 * XXX: should reuse. */
	ret = EDMA3_DRV_freeChannel (exfer->hEdma, exfer->chId);
	if (ret != EDMA3_DRV_SOK) {
		printf("edma3_test: EDMA3_DRV_freeChannel() FAILED, " \
		       "error code: %d\n", ret);
		assert(0);
	}

	// XXX free xfer
	return EDMA3_DRV_SOK;
}

// wait for a xfer to finish without freeing the channel
EDMA3_DRV_Result edma3_wait_xfer1(void *xfer)
{
	EDMA3_DRV_Result ret;
	//	struct timeval end;
	struct edma_xfer *exfer = (struct edma_xfer *)(xfer); // handy
	assert(exfer);

	// does the following func wait on intermediate irq? :(
	ret = EDMA3_DRV_waitAndClearTcc(exfer->hEdma, exfer->tcc); //should not use chId
	//	gettimeofday(&end, NULL);

	//	D("Transfer completed! %ld us",
	//	  (end.tv_sec * 1000000 + end.tv_usec)
	//	  - (exfer->start.tv_sec * 1000000 + exfer->start.tv_usec));

	assert(ret == EDMA3_DRV_SOK);

	// XXX free xfer
	return EDMA3_DRV_SOK;
}

// return 0 on success
int edma3_wait_gather(struct edma_xfer_scatter *scatter_xfer)
{
	EDMA3_DRV_Result result;
	//	struct timeval end;

	assert(scatter_xfer);
	struct edma_xfer_scatter *xfer = (struct edma_xfer_scatter *)scatter_xfer;

	assert(xfer->chinfo);

	result = EDMA3_DRV_waitAndClearTcc(xfer->chinfo->hedma, xfer->chinfo->tcc);
	checkret();

#if 0

	gettimeofday(&end, NULL);
	V("Transfer completed! %ld us",
	  (end.tv_sec * 1000000 + end.tv_usec)
	  - (xfer->start.tv_sec * 1000000 + xfer->start.tv_usec));
#endif

	free(scatter_xfer);
	return 0;
}

// return 0 on success
// we also have to restore the PSETs --
// -- the phys channel, which is clobbered
// -- the last PSET used in the previous xfer.

int edma3_wait_gather_preloaded(struct edma_xfer_scatter *scatter_xfer)
{
	EDMA3_DRV_Result result;
	//	struct timeval end;

	assert(scatter_xfer);
	struct edma_xfer_scatter *xfer = (struct edma_xfer_scatter *)scatter_xfer;

	assert(xfer->chinfo);
	//	printf("xfer->chinfo->hedma %08x xfer->chinfo->chid %d xfer->chinfo->tcc %x",
	//			(unsigned int)xfer->chinfo->hedma, xfer->chinfo->ch, xfer->chinfo->tcc);

	result = EDMA3_DRV_waitAndClearTcc(xfer->chinfo->hedma, xfer->chinfo->tcc);
	checkret();
	//	gettimeofday(&end, NULL);

	//	V("Transfer completed! %ld us",
	//	  (end.tv_sec * 1000000 + end.tv_usec)
	//	  - (xfer->start.tv_sec * 1000000 + xfer->start.tv_usec));

	// ---- restored the clobbered PSETs to the preloaded default values.
	// the phys channel PSET, was zeroed by HW
	xfer->chinfo->pset->linkAddr = xfer->chinfo->link;
	result = EDMA3_DRV_setPaRAM(xfer->chinfo->hedma, xfer->chinfo->ch, xfer->chinfo->pset);
	checkret();
	// the last PSET --- link and opt fields
	// XXX only has to fix Link & Opt. dont set the entire PSET
	if (xfer->cnt > 1)
	{
		xfer->chinfo->pset->linkAddr = xfer->chinfo->links[xfer->cnt-2];
		result = EDMA3_DRV_setPaRAM(xfer->chinfo->hedma,
		                            xfer->chinfo->logic_chs[xfer->cnt - 2],
		                            xfer->chinfo->pset);
		checkret();
	}
	xfer->chinfo->pset->linkAddr = 0;

	free(scatter_xfer);
	return 0;
}

// nonblocking
// return: true if xfer is done; false otherwise
int edma3_check_xfer(void *xfer)
{
	EDMA3_DRV_Result ret;
	unsigned short status;
	struct edma_xfer *exfer = (struct edma_xfer *)(xfer); // handy

	assert(exfer);

	ret = EDMA3_DRV_checkAndClearTcc(exfer->hEdma, exfer->chId, &status);
	assert(ret == EDMA3_DRV_SOK && "EDMA3_DRV_checkAndClearTcc failed");

	return (int)status;
}

/* to keep edma3 lld happy */
EDMA3_DRV_Result edma3OsSemCreate()
{
	sem_init(&mutex, 0, 1);
	return EDMA3_DRV_SOK;
}

EDMA3_DRV_Result edma3OsSemDelete()
{
	sem_destroy(&mutex);
	return EDMA3_DRV_SOK;
}

EDMA3_DRV_Result edma3OsSemTake(EDMA3_OS_Sem_Handle hSem, int32_t mSecTimeout)
{
	sem_post(&mutex);
	return EDMA3_DRV_SOK;
}

EDMA3_DRV_Result edma3OsSemGive(EDMA3_OS_Sem_Handle hSem)
{
	sem_wait(&mutex);
	return EDMA3_DRV_SOK;
}

void edma3OsProtectEntry (unsigned int edma3InstanceId, int level, unsigned int *intState)
{}

void edma3OsProtectExit (unsigned int edma3InstanceId,
                         int level, unsigned int intState)
{}

