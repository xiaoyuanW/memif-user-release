/*
 * cmem.h
 *
 *  Created on: May 28, 2015
 *      Author: xzl
 *
 *  assert() is heavily used. so user often needs no error checking.
 */

#ifndef CMEM_H_
#define CMEM_H_

#include <ti/cmem.h>

/* some size defs */
#ifndef SZ_1M
#define SZ_1M 			(1024 * 1024)
#endif

#define K2H_MSMC_SIZE	(6 * SZ_1M)

/* mem types */
typedef enum {mt_ddr, mt_msmc_cmem, mt_msmc_mmap, mt_ddr_cmem} memtype_t;
extern const char *mt_desc[];

/* return 0 on success */
int cmem_init(void);

/* @dma_ddr [out]. will automatically call cmem_init() if not invoked before. */
void *cmem_alloc_ddr(unsigned long size, unsigned long * dma_addr);
void *cmem_alloc_msmc(unsigned long size, unsigned long * dma_addr);
void cmem_cleanup(void);

/* using heuristics to find ddr & msmc blocks
 *
 * ddr: 512M beyond the ddr phys base (32bit: 8000:0000; 40bit: 8:0000:0000)
 * msmc: phys < 0x1000:0000
 *
 * the arg should be 32bit based phys addr, as used by cmem APIs
 */
#define is_phys_ddr(phys) 		(phys > 0x20000000)
#define is_phys_msmc(phys)		(phys < 0x10000000)

#define phys32_to_dma(phys)  (is_phys_ddr(phys) ? (phys + 0x80000000) : phys)

#endif /* CMEM_H_ */
