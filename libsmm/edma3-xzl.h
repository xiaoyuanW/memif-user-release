#ifndef _EDMA3_XZL
#define _EDMA3_XZL

#include <ti/sdo/edma3/drv/edma3_drv.h>  // xzl
#include "misc-xzl.h"

EDMA3_DRV_Handle edma3_init(void);

EDMA3_DRV_Result edma3_fini(void);

struct edma_xfer;
struct edma_xfer_scatter;

// repeat xfer using edma3's link functionalities
// @cnt: how many times to repeat
int edma3_start_xfer_multiple(EDMA3_DRV_Handle hEdma, unsigned long src,
		unsigned long dest, unsigned long size, int cnt, void **xfer);
int edma3_start_xfer_multiple_selfchaining(EDMA3_DRV_Handle hEdma, unsigned long src,
        unsigned long dest, unsigned long size, int count, void **xfer);

// this should be thread-safe
EDMA3_DRV_Result edma3_start_xfer_once(EDMA3_DRV_Handle hEdma, unsigned long src,
		unsigned long dest, unsigned long size, int cc, void **xfer);
EDMA3_DRV_Result edma3_wait_xfer(void *xfer);

struct dma_ch_info *edma3_allocate_channel(EDMA3_DRV_Handle hEdma);
struct dma_ch_info *edma3_allocate_channel_linked(EDMA3_DRV_Handle hEdma, int logic_cnt);
// can free both linked/single
void edma3_free_channel(struct dma_ch_info *ch_handle);

EDMA3_DRV_Result edma3_start_xfer_once1
					(void *ch_handle,
							unsigned long src,
							unsigned long dest, unsigned long size, struct edma_xfer **xfer);
EDMA3_DRV_Result edma3_wait_xfer1(void *xfer);

int edma3_check_xfer(void *xfer);


struct edma_xfer_scatter *edma3_start_gather(void *handle, unsigned long *srcs, int src_stride,
		unsigned long *voffset, int voffset_stride,
		unsigned long *sizes, int size_stride,
		unsigned long cnt, unsigned long dest);
int edma3_wait_gather(struct edma_xfer_scatter *scatter_xfer);

// @handle: dma_ch_info* returned by edma3_start_gather_preloaded()
struct edma_xfer_scatter *edma3_start_gather_preloaded(void *handle,
		unsigned long *srcs, int src_stride,
		unsigned long *voffsets, int voffset_stride,
		unsigned long cnt, unsigned long dest);
int preload_psets(struct dma_ch_info * chinfo, unsigned long size);
int edma3_wait_gather_preloaded(struct edma_xfer_scatter *scatter_xfer);

/* ---- internal use --- */
// all info about an open channel
struct dma_ch_info {
	EDMA3_DRV_Handle hedma;
	int ch;
	int tcc;
	unsigned short link; // the main PSET's link, should point to logic channel 0
	// the pre-allocated logic channels (PSETs)
	unsigned int *logic_chs;
	// logic channels' links
	unsigned short *links;
	int logic_cnt;
	// ---- preloaded PSETs ----
	// cached, default pset
	EDMA3_DRV_PaRAMRegs *pset;
	unsigned long size; 	// unified xfer size
	// cached parameters for the last pset in a gather
	unsigned int last_brcnt_link;
	unsigned int last_opt;
};

#endif
