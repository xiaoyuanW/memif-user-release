/* xzl -- from numactl project

	Copyright:

	numactl and the demo programs are under the GNU General Public License, v.2
	libnuma is under the GNU Lesser General Public License, v2.1.

	The manpages are under the same license as the Linux manpages (see the files)

	numademo links with a library derived from the C version of STREAM
	by John D. McCalpin and Joe R. Zagar for one sub benchmark. See stream_lib.c
	for the license. In particular when you publish numademo output
	you might need to pay attention there or filter out the STREAM results.

	It also uses a public domain Mersenne Twister implementation from
	Michael Brundage.

	Version 2.0.10-rc2: (C)2014 SGI

	Author:
	Andi Kleen, SUSE Labs

	Version 2.0.0 by Cliff Wickman, Christoph Lameter and Lee Schermerhorn
	cpw@sgi.com clameter@sgi.com lee.schermerhorn@hp.com

 *
 * Clear the CPU cache for benchmark purposes. Pretty simple minded.
 * Might not work in some complex cache topologies.
 * When you switch CPUs it's a good idea to clear the cache after testing
 * too.
 */
#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __arm__
#include <sys/syscall.h>
#endif
#include "clearcache.h"

unsigned cache_size(void)
{
	unsigned cs = 0;
#ifdef _SC_LEVEL1_DCACHE_SIZE
	cs += sysconf(_SC_LEVEL1_DCACHE_SIZE);
#endif
#ifdef _SC_LEVEL2_DCACHE_SIZE
	cs += sysconf(_SC_LEVEL2_DCACHE_SIZE);
#endif
#ifdef _SC_LEVEL3_DCACHE_SIZE
	cs += sysconf(_SC_LEVEL3_DCACHE_SIZE);
#endif
#ifdef _SC_LEVEL4_DCACHE_SIZE
	cs += sysconf(_SC_LEVEL4_DCACHE_SIZE);
#endif
	if (cs == 0) {
		static int warned;
		if (!warned) {
			printf("Cannot determine CPU cache size\n");
			warned = 1;
		}
		cs = 64*1024*1024;
	}
	cs *= 2; /* safety factor */

	return cs;
}

void fallback_clearcache(void)
{
	static unsigned char *clearmem;
	unsigned cs = cache_size();
	unsigned i;

	if (!clearmem)
		clearmem = malloc(cs);
	if (!clearmem) {
		printf("Warning: cannot allocate %u bytes of clear cache buffer\n", cs);
		return;
	}
	for (i = 0; i < cs; i += 32)
		clearmem[i] = 1;
}

// xzl: this should wb + inv.
void clearcache(unsigned char *mem, unsigned size)
{
	//printf("clearing $....\n");

#if defined(__i386__) || defined(__x86_64__)
	unsigned i, cl, eax, feat;
	/* get clflush unit and feature */
	asm("cpuid" : "=a" (eax), "=b" (cl), "=d" (feat) : "0" (1) : "cx");
	if (!(feat & (1 << 19)))
		fallback_clearcache();
	cl = ((cl >> 8) & 0xff) * 8;
	for (i = 0; i < size; i += cl)
		asm("clflush %0" :: "m" (mem[i]));
#elif defined(__ia64__)
        unsigned long cl, endcl;
        // flush probable 128 byte cache lines (but possibly 64 bytes)
        cl = (unsigned long)mem;
        endcl = (unsigned long)(mem + (size-1));
        for (; cl <= endcl; cl += 64)
                asm ("fc %0" :: "r"(cl) : "memory" );
#elif defined (__arm__) /* xzl */
	/* xzl: the "fallback" seems to generate over-optimistic results for STREAM.
	   the following should go to __ARM_NR_cacheflush().
	   how well this work?

	   For memif on ks2, we end up not flushing cache, as ARMv7 has no aliasing
	   and edma3 is coherent.
	*/

    //	__clear_cache(mem, mem+size);

    /*
	   see kernel/trap.c: do_cache_op() and flush_cache_user_range()
	*/
	syscall(__ARM_NR_cacheflush, mem, mem + size, 0);
#else
#warning "Consider adding a clearcache implementation for your architecture"
	fallback_clearcache();
#endif
}

// xzl: TODO: add flushcache support?
