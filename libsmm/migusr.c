/*
 * migusr.c
 *
 *  Created on: Aug 6, 2015
 *      Author: xzl
 *
 *  User API
 */

#define _BSD_SOURCE

//#define K2_DEBUG_VERBOSE 1
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
#include <poll.h>

#include "mig.h"
#include "log-xzl.h"
#include "measure.h"
#include "misc-xzl.h"

mig_desc *memif_start_replication(int fd, mig_region *r, void *src,
		void *dst, unsigned order)
{
	mig_desc *desc;
	color_t clr;

	assert(r);
	assert(fd > 0);

	desc = freelist_remove(r);
	assert(desc);
	desc->virt_base = virt_to_base(src);
	desc->virt_base_dest = virt_to_base(dst);
	desc->order = order;

//	I("src %08x dst %08x", desc->virt_base, desc->virt_base_dest);

	/* submit the request */
	clr = mig_enqueue(r, &r->qreq, desc);
	if (clr == COLOR_USR) {
		int cmd = MIG_MOVE_SINGLE_NOREMAP;
		int ret;

		ret = write(fd, &cmd, sizeof(int));
		if (ret < 0) {
			E("return value %d", ret);
		}
	}

	return desc;
}

/* @nr: device nr, 0--3
 * @region: OUT, pointer to the mmap'd region.
 *
 * adapted from @kernel_driver_test()
 *
 * return: the fd */

static const char * fns[] = {
		"/sys/kernel/debug/migif",
		"/sys/kernel/debug/migif1",
		"/sys/kernel/debug/migif2",
		"/sys/kernel/debug/migif3"
};

int memif_open(int nr, mig_region **region)
{
	int fd;
	char * address = NULL;

	assert(nr >= 0 && nr <= 3);

	if ((fd = open(fns[nr], O_RDWR)) < 0) {
		perror("open");
		return -1;
	}

	/* the actual size does not matter */
	address = mmap(NULL, 3 * PAGE_SIZE, PROT_READ|PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (address == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	/* now the driver should have d/s ready; directly use them */
	*region = (mig_region *)address;
	V("dev%d: mmap'd region is %08x ndescs %d",
			nr, (uint32_t)(*region), (*region)->ndescs);

	return fd;
}
