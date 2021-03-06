/*
 * measure.c
 *
 * K2 project, 2013
 *
 * May 2015. ported to userspace.
 * Contact: Felix Xiaozhu Lin <linxz02@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * known issues:
 * - on M3 gettimeofday() does not work properly.
 * - even with HAS_LOCK, still crashes the kernel in multicore case.
 * (strnlen?)
 */

#ifdef __KERNEL__
/* what to include? */
#else
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

#include "measure.h"
#include "log-xzl.h"

#ifndef print_to_tracebuffer
#define print_to_tracebuffer(fmt, args...) printf(fmt, ##args)
#endif

#ifdef __KERNEL__
#define HAS_LOCK 1
#endif

struct _perf_stat k2_perf_stat = {0};


/* -------------------------------------------------
 * take timestamp and save msg. minimize overhead
 * ------------------------------------------------- */
#define NR_SAMPLES 256
struct measure_sample {
	const char *msg;
	u32 ts; 		/* in cycles. from hw counter. */
#ifdef CONFIG_ARCH_OMAP4
	u32 tod;		/* in us. from gettimeofday(). */
#endif
	struct _perf_stat perf_stat; /* snapshot of k2_perf_stat */
};
static int count = 0; /* # of samples since last flush */
static struct measure_sample samples[NR_SAMPLES];

#if HAS_LOCK
static DEFINE_SPINLOCK(measure_lock); /* for the queue of samples */
#endif

/* XXX lock, see ctm.h  */
void k2_perf_stat_snapshot(struct _perf_stat *out)
{
	memcpy(out, &k2_perf_stat, sizeof(k2_perf_stat));
}

/* drop all samples without looking at them (e.g.,
 * they may include invalid pointers).
 * return the # of samples dropped.
 * XXX make this atomic?
 */
int k2_measure_clean(void)
{
	int ret = count;
	count = 0;
	return ret;
}
#ifdef __KERNEL__
EXPORT_SYMBOL(k2_measure_clean);
#endif

/* take a snapshot of the current sw perf cnt, timestamped .
   perf sensitive.

   Since perfcnt of A9 won't tick when the core is idle,
   for A9 every measure we take two readings: perfcnt and gettimeofday().
   The latter comes from a 32KHz timer.

   return the current time (in us) for being handy.
 */
u32 k2_measure(const char *msg)
{
#if HAS_LOCK
	unsigned long flags;
#endif
	struct timeval stamp;
	u32 tod_us;
	int i = count % NR_SAMPLES;

#if HAS_LOCK
	spin_lock_irqsave(&measure_lock, flags);
#endif

	if (i == NR_SAMPLES - 1) {
		/* we've reached tail (again) */
		/*
		k2_PRINT("sample overflow. bug!\n");
		k2_crash();
		 */
		/* better not PRINT; might distort measurements... */
		W("WARN - sample overflow.\n");
		k2_perf_stat.mb_overflow ++;
	}

	samples[i].msg = msg;
//	samples[i].ts = k2_get_cyclecount();	// not very useful in user space due to ctx switch

	k2_gettimeofday(&stamp);
	tod_us = stamp.tv_sec * USEC_PER_SEC + stamp.tv_usec;

#ifdef CONFIG_ARCH_OMAP4
	samples[i].tod = tod_us;
#endif

	/* take a snapshot of the current perf stat */
	memcpy(&samples[i].perf_stat, &k2_perf_stat, sizeof(k2_perf_stat));

	count ++;

#if HAS_LOCK
	spin_unlock_irqrestore(&measure_lock, flags);
#endif

	return tod_us;
}
#ifdef __KERNEL__
EXPORT_SYMBOL(k2_measure);
#endif

///* Obsoleted by k2_flush_measure_format(). NO LONGER MAINTAINED.
// * Keep it here for compatible with prior measurements.
// *
// * Print and clean all saved measurements. Non perf sensitive
// */
//void k2_flush_measure(void)
//{
//	int i;
//	/* dump all samples in cycles. The way we interpret timestamps as
//	 * cycles depends on the hw */
//	for (i = 0; i < count; i++) {
//#ifdef CONFIG_ARCH_OMAP_M3
//		k2_PRINT("\"%s\" delta %u us (now: %u) %u %u %u\n",
//				samples[i].msg,
//				i == 0 ? -9999 : (samples[i].ts - samples[i-1].ts) / (CTM_FREQ / 1000 / 1000),
//				samples[i].ts / (CTM_FREQ / 1000 / 1000),
//				samples[i].perf_stat.cc_req, samples[i].perf_stat.cc_serv,
//				samples[i].perf_stat.m3_l1_swap);
//#elif defined (CONFIG_ARCH_OMAP4)
//		k2_PRINT("\"%s\" delta %u 64cycles (now: %u)\n",
//						samples[i].msg,
//						i == 0 ? -9999 : (samples[i].ts - samples[i-1].ts),
//						samples[i].ts);
//#else
//#error "unsupported arch"
//#endif
//	}
//
//#ifdef CONFIG_ARCH_OMAP_M3
//		k2_PRINT(" TOTAL: %u us \n",
//				(samples[i-1].ts - samples[0].ts) / (CTM_FREQ / 1000 / 1000));
//#elif defined (CONFIG_ARCH_OMAP4)
//		k2_PRINT(" TOTAL: %u 64cycles \n", samples[i-1].ts - samples[0].ts);
//#else
//#error "unsupported arch"
//#endif
//
//  /* For A9, dump extra timestamps taken do_gettimeofday */
//#ifdef CONFIG_ARCH_OMAP4
//  k2_PRINT("do_gettimeofday()\n");
//  for (i = 0; i < count; i++) {
//		k2_PRINT(" \"%s\" delta %u us (now: %u)\n",
//						samples[i].msg,
//						i == 0 ? -9999 : (samples[i].tod - samples[i-1].tod),
//						samples[i].tod);
//	}
//	k2_PRINT(" TOTAL: %u us \n", samples[i-1].tod - samples[0].tod);
//#endif
//
//	k2_PRINT("---------------------------------------------------------------\n");
//	count = 0;
//}

void k2_flush_measure_format(void)
{
	int i = 0, j, start, end;

#if HAS_LOCK
	unsigned long flags;
	spin_lock_irqsave(&measure_lock, flags);
#endif

	/* header */
	print_to_tracebuffer("--------------------%s------#samples=%d---------------\n",
			__func__, count);
#ifdef CONFIG_ARCH_OMAP_M3
	print_to_tracebuffer("%40s %6s %10s "
			"%6s %6s %6s %6s %6s"
			"%6s %6s %6s %6s %6s\n",
			"msg", "delta", "now",
			"cc_req", "cc_serv", "toarm", "tothu", "l1swap",
			"bus", "hard", "usage", "mm", "mb_ov");
#elif defined (CONFIG_ARCH_OMAP4)
	print_to_tracebuffer("%20s %4s %4s"
			"%15s %15s %8s %8s %8s\n",
			"msg", "delta", "now",
			"delta(tod/us)", "now(tod)", "cc_req", "cc_serv", "mb_ov");
#else
#error "unsupported arch"
#endif

	/* dump all samples in cycles. The way we interpret timestamps as
	 * cycles depends on the hw */

	/* if the sample buffer has wrapped, we find the head and start from it */
	if (count > NR_SAMPLES) { /* we overflew */
		start = count % NR_SAMPLES;
		end = start + NR_SAMPLES;
	} else {
		start = 0;
		end = count;
	}

	for (j = start; j < end; j++) {
		i = j % NR_SAMPLES;
		if (i == 0) print_to_tracebuffer("*"); /* dbg */
#ifdef CONFIG_ARCH_OMAP_M3
		print_to_tracebuffer("%40s %6u %10u "
				"%6u %6u %6u %6u %6u"
				"%6u %6u %6u %6u %6u\n",
				samples[i].msg,
				i == 0 ? 0 : (samples[i].ts - samples[i-1].ts) / (CTM_FREQ / 1000 / 1000),
				samples[i].ts / (CTM_FREQ / 1000 / 1000),
				samples[i].perf_stat.cc_req, samples[i].perf_stat.cc_serv,
				samples[i].perf_stat.m3_redirect_toarm, samples[i].perf_stat.m3_redirect_tothumb,
				samples[i].perf_stat.m3_l1_swap,
				samples[i].perf_stat.m3_busfault, samples[i].perf_stat.m3_hardfault,
				samples[i].perf_stat.m3_usagefault, samples[i].perf_stat.m3_memmanagefault,
				samples[i].perf_stat.mb_overflow);

#elif defined (CONFIG_ARCH_OMAP4)
		print_to_tracebuffer("%20s %4u %4u"
				"%15u %15u %8u %8u %8u\n",
				samples[i].msg,
				i == 0 ? 0 : (samples[i].ts - samples[i-1].ts), samples[i].ts,
				i == 0 ? 0 : (samples[i].tod - samples[i-1].tod), samples[i].tod,
				samples[i].perf_stat.cc_req, samples[i].perf_stat.cc_serv,
				samples[i].perf_stat.mb_overflow);
#else
#error "unsupported arch"
#endif
	}

#ifdef CONFIG_ARCH_OMAP_M3
	print_to_tracebuffer(" TOTAL: %u us \n",
			(samples[i-1].ts - samples[0].ts) / (CTM_FREQ / 1000 / 1000));
#elif defined (CONFIG_ARCH_OMAP4)
	/* For A9, dump extra timestamps taken do_gettimeofday */
	print_to_tracebuffer(" TOTAL: %u 64cycles %u us(gettimeofday)\n",
				samples[i-1].ts - samples[0].ts,
				samples[i-1].tod - samples[0].tod);
#else
#error "unsupported arch"
#endif

	print_to_tracebuffer("---------------------------------------------------------------\n");
	count = 0;

#if HAS_LOCK
	spin_unlock_irqrestore(&measure_lock, flags);
#endif

}
#ifdef __KERNEL__
EXPORT_SYMBOL(k2_flush_measure_format);
#endif

//// calibrate the time spent on k2 measure
//void k2_calibrate()
//{
//  u32 before, after, delta;
//
//  before = k2_get_cyclecount();
//  k2_measure("for k2 measurement calibration");
//  after = k2_get_cyclecount();
//  delta = after - before;
//  /* xzl: why this can take up to 2us? k2_get_cyclecount() expensive? */
//
//#ifdef CONFIG_ARCH_OMAP_M3
//  k2_PRINT("k2_measure() takes %u us\n", delta / (CTM_FREQ / 1000 / 1000));
//#elif defined (CONFIG_ARCH_OMAP4)
//  k2_PRINT("k2_measure() takes %u 64cycles\n", delta);
//#endif
//
//  count = 0;
//}
//
//#ifdef CONFIG_ARCH_OMAP4
///* ----------------- simple perf cnt, see ------------------------
// * http://stackoverflow.com/questions/3247373/how-to-measure-program-execution-time-in-arm-cortex-a8-processor
// * XXX move this to a sep file
// */
////static inline
//void init_perfcounters (int32_t do_reset, int32_t enable_divider)
//{
//  // in general enable all counters (including cycle counter)
//  int32_t value = 1;
//
//  // peform reset:
//  if (do_reset)
//  {
//    value |= 2;     // reset all counters to zero.
//    value |= 4;     // reset cycle counter to zero.
//  }
//
//  if (enable_divider)
//    value |= 8;     // enable "by 64" divider for CCNT.
//
//  value |= 16;
//
//  // program the performance-counter control-register:
//  asm volatile ("MCR p15, 0, %0, c9, c12, 0\t\n" :: "r"(value));
//
//  // enable all counters:
//  asm volatile ("MCR p15, 0, %0, c9, c12, 1\t\n" :: "r"(0x8000000f));
//
//  // clear overflows:
//  asm volatile ("MCR p15, 0, %0, c9, c12, 3\t\n" :: "r"(0x8000000f));
//}
//
//static int __init setup_perfcnt(void)
//{
//    /* init perf cnt */
//	k2_Print("done\n");
//    init_perfcounters(1, 1);
//    return 0;
//}
//late_initcall(setup_perfcnt);  /* don't get override */

