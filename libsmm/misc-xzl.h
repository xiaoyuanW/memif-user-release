#ifndef _MISC_XZL
#define _MISC_XZL

#ifndef __KERNEL__	/* these things are defined in kernel already */
#define SZ_1K           (1024)
#define SZ_4K           (4 * 1024)
#define SZ_16K           (16 * 1024)
#define SZ_32K          (32 * 1024)
#define SZ_64K          (64 * 1024)
#define SZ_1M           (1024 * 1024)
#define SZ_1G           (1024 * 1024 * 1024)
#endif

#ifndef PAGE_SIZE
#define PAGE_SIZE SZ_4K
#endif

#endif // _MISC_XZL
