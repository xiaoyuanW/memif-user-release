/*
 * Memory Migration functionality - linux/mm/migration.c
 *
 * Copyright (C) 2006 Silicon Graphics, Inc., Christoph Lameter
 *
 * Page migration was first developed in the context of the memory hotplug
 * project. The main authors of the migration code are:
 *
 * IWAMOTO Toshihiro <iwamoto@valinux.co.jp>
 * Hirokazu Takahashi <taka@valinux.co.jp>
 * Dave Hansen <haveblue@us.ibm.com>
 * Christoph Lameter
 *
 *
 * xzl: for the memif project. 2015.
 *
 * This probably shouldn't be built as a kernel module, as it seems to use a
 * lot of unexported functions.  However, built-in requires us to build in the
 * edma drivers etc, which may make hacking harder...
 *
 * We keep migrate.c (the stock migration functions) and migrate-ks2.c in the
 * same kernel tree so that we can easily test them both. We comment out
 * duplicate functions from migrate-ks2.c.
 *
 * Multicore: A user enters the driver to call kernel_flush_work. The CPU id is
 * saved to @table by issue_one_desc_XXXX().
 *
 * The irq happens on core 0, and examines table->user_cpu to decide which
 * core's kernel thread to pass the work on.
 *
 * Once a per-core kernel thread is activated, all remaining work is done on
 * that core.
 *
 * Each dma channel is protected by virt dma's per-chan spinlock.  One lock is
 * added to edma3_drv to protect the controller-wide regs (e.g., trigger xfer,
 * mask/unmask irq). Strangely, this lock does not exist in the stock driver.
 */

#define K2_NO_MEASUREMENT 1

//#define K2_NO_DEBUG 1
#define K2_LOCAL_DEBUG_LEVEL 40

#include <linux/migrate.h>
#include <linux/export.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include <linux/mm_inline.h>
#include <linux/nsproxy.h>
#include <linux/pagevec.h>
#include <linux/ksm.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/writeback.h>
#include <linux/mempolicy.h>
#include <linux/vmalloc.h>
#include <linux/security.h>
#include <linux/memcontrol.h>
#include <linux/syscalls.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>
#include <linux/gfp.h>
#include <linux/balloon_compaction.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/poll.h>
#include <linux/delay.h>
#include <linux/backing-dev.h>
#include <linux/mmu_notifier.h>
#include <linux/vmstat.h>
/* for dma */
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/platform_data/edma.h>

#include <asm/tlbflush.h>
#include <linux/ktime.h>
#include <linux/hugetlb.h>
#include <asm/atomic.h>
#include <linux/semaphore.h> 

//#define CREATE_TRACE_POINTS
//#include <trace/events/migrate.h>

/* xzl: if to compile this file as a standalone kernel, copy this header file
 * to the module source dir from mm/internal.h */

#ifdef __MODULE__
#include "mm-internal.h"
#else
#include "internal.h"
#endif

#include "measure.h"
#include "log-xzl.h"
#include "mig.h"

/* the layout of migregion and device info. see if_open() */
#define NPAGEORDER 2
#define NPAGES (1 << NPAGEORDER)
#define N_MIGREGION_PAGES (NPAGES - 1)

//#define NR_DEVICES	4
#define NR_DEVICES 16

#define MULTI_CHAN_SERIAL 0 // 1: mig/cp using multiple channels serially, 0: parallel

/* Using dummy transfers to emulate huge pages so that we can measure the
 * performance impact of the huge pages. */
//#define EMU_HUGETLB 	1

#ifdef EMU_HUGETLB
/* equals to how many 4K pages?
 * order 4  -- 64K page (16x4k)
 * order 9  -- 2048K page (512*4k)
 */
#define EMU_HUGETLB_PG_ORDER	9
#define EMU_HUGETLB_PG_SIZE		((1<<EMU_HUGETLB_PG_ORDER) * PAGE_SIZE)

/* dummy pages from the two nodes. pre allocated to save runtime.
 * note this becomes non-thread safe.
 */
static struct page *the_ddr_pages = 0;
static unsigned long the_dummy_dma[2] = {0};
#endif

#define HUGE_PAGE_SIZE (2 * 1024 * 1024) //hym: for huge page
int huge_page; //global variable, bad idea...

//static atomic_t my_counter; //hym
static atomic_t my_counter = ATOMIC_INIT(NR_DEVICES);
ktime_t cmplet_time[NR_DEVICES];
ktime_t start_transfer[NR_DEVICES];
struct semaphore sem;
/*
 * One channel per device file.
 * these chans are allocated during initialization, and assigned to
 * files when files are opened.
 * this way we keep the state inside each chan.
 */
static struct dma_chan *chans[NR_DEVICES] = {NULL};

/* per file d/s. this is pointed by filp->private_data.
 * in allocating it, we give this d/s one page right after the file's
 * @mig_region. */
struct mmap_info
{
//    char *data;	// xzl: points to the kernel pages that back mig_region
	struct dma_chan *chan;
	mig_region *region;
    int reference;
    wait_queue_head_t wait_queue;
    struct semaphore sem;  /* for fd protection */
};

/* info@ is right after the mig_region. see if_open() */
#define region_to_mmap_info(r) \
	(struct mmap_info *) ((unsigned long)r + PAGE_SIZE * (NPAGES-1))

/* the desc for an outstanding xfer */
struct mig_pg_desc {
	struct page *newpage;
	struct page *page;
	int remap_swapcache;
	struct anon_vma *anon_vma;
	struct mem_cgroup *mem;
	mig_desc	*mig;
	pte_t *ptep, pte;
	/* mig status for returning */
	int status;
};

struct mig_table;

typedef struct mig_table *issue_one_desc_t(mig_region *,
		mig_desc *, int, struct mm_struct *);

/* every mig_table represents an activity in which the driver pulls descs
 * from a region's request queue, assembles a dma sg list, and submit the
 * list to the dma engine driver.
 *
 * the sg list is executed by dmaengine as one xfer, which generates one
 * irq.
 *
 * 1 migration : N mig descs from request queue
 * 1 mig desc : N sg elements / pages (each mm_desc)
 *
 * (we may opportunistically coalesce sg elements that are physically
 * contig, but this probably won't help much)
 */
struct mig_table {
	mig_region * region; /* has to be first. see flush_single_qreq() */

	/* all from one process. multiplexing happens within dma driver. */
	struct mm_struct *mm;

	/* have to save it here, will be used by @work below to submit the
	 * next desc */
	issue_one_desc_t *issue_one_desc_func;

	/* has to be part of @mig_table -- the kernel thread will use
	 * container_of(work) to locate @mig_table
	 */
	struct work_struct work;

	/* we don't keep the mapping between migdesc and mm_desc.
	 * when the sg list is completed, we simply grab @num_mig_descs from the
	 * req head and move them to other queue etc.
	 */
	int num_mig_descs;
	int num_pages;

	/* isolated list for page->lru. we need the list_head survive across
	 * a dma xfer. */
	struct list_head pagelist;

	union {
		int tx_id;  // debug
		unsigned int user_cpu; /* the cpu id of the original caller */
	};

	mig_desc	*migdesc;

	/* has to come last */
	struct mig_pg_desc pg_desc[MAX_NR_SG];	/* end with mm_desc.page == NULL */
};

//#define MIGTABLE_PAGE_ORDER	2	/* 4 pages, 16k */
#define MIGTABLE_PAGE_ORDER 4           //modify by hym

#define last_pg_desc(d) (d.page == NULL)

/* thanks to the colored queue, the user and the kernel thread will not
 * race on assembling dma transfers. thus, multiple variables can be made
 * static and per-core specific, so that we don't have to kmalloc/kfree them
 * at run time.
 *
 * this does not include mig_table: it is possible that we assemble one
 * mig_table while there's another outstanding. thus we have to dynamically
 * allocate mig_table.
 */
struct mig_device {
	struct scatterlist sg_src[MAX_NR_SG];
	struct scatterlist sg_dst[MAX_NR_SG];
};

static int flush_single_qreq(mig_region *r, int user, struct mm_struct *mm,
		int parent);
//static int flush_single_qreq_noremap(mig_region *r, int user, struct mm_struct *mm,
//		int parent);
static int flush_single_qreq_noremap2(mig_region *r, int user, struct mm_struct *mm,
		int parent);

static struct mig_table *issue_one_desc_mig(mig_region *r,
		mig_desc *desc, int do_irq, struct mm_struct *mm);

static struct mig_table *issue_one_desc_copy(mig_region *r,
		mig_desc *desc, int do_irq, struct mm_struct *mm);

static void kernel_flush_work(struct work_struct *work);

/* when to release the channel? */
static struct dma_chan *init_dma_chan(void)
{
	dma_cap_mask_t mask;
	int ch_num = 16;
	struct dma_chan *chan;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);
	//dma_cap_set(DMA_SG, mask);

	/* any memcpy channel. no filter provided */
	//chan = dma_request_channel(mask, NULL, &ch_num);
	chan = dma_request_channel(mask, NULL, NULL); //hym
	if (chan) {
		printk("grab a dmaengine chan %s\n",dma_chan_name(chan));
		return chan;
	}
	else {
		E("FAILED to grab a dmaengine chan\n");
		return NULL;
	}
}

/* debug, check the vma. modeled after rmap_walk_anon() */
static void check_vma(struct page *page)
{
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	pgoff_t pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);

	anon_vma = page_anon_vma(page);
	if (!anon_vma) {
		I("anon_vma does not exist");
		return;
	}
	anon_vma_lock_read(anon_vma);
	anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff, pgoff) {
		struct vm_area_struct *vma = avc->vma;
		I("page %pK vma %pK. mm %pK (mapcount %d users %d count %d)",
				page, vma, vma->vm_mm, vma->vm_mm->map_count,
				vma->vm_mm->mm_users.counter, vma->vm_mm->mm_count.counter);
	}
	anon_vma_unlock_read(anon_vma);
}

#if 0	//xzl: use the defs in migrate.c. Avoid duplication.
/*
 * migrate_prep() needs to be called before we start compiling a list of pages
 * to be migrated using isolate_lru_page(). If scheduling work on other CPUs is
 * undesirable, use migrate_prep_local()
 */
int migrate_prep(void)
{
	/*
	 * Clear the LRU lists so pages can be isolated.
	 * Note that pages may be moved off the LRU after we have
	 * drained them. Those pages will fail to migrate like other
	 * pages that may be busy.
	 */
	lru_add_drain_all();

	return 0;
}

/* Do the necessary work of migrate_prep but not if it involves other CPUs */
int migrate_prep_local(void)
{
	lru_add_drain();

	return 0;
}

/*
 * Add isolated pages on the list back to the LRU under page lock
 * to avoid leaking evictable pages back onto unevictable list.
 */
void putback_lru_pages(struct list_head *l)
{
	struct page *page;
	struct page *page2;

	list_for_each_entry_safe(page, page2, l, lru) {
		list_del(&page->lru);
		dec_zone_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
			putback_lru_page(page);
	}
}

/*
 * Put previously isolated pages back onto the appropriate lists
 * from where they were once taken off for compaction/migration.
 *
 * This function shall be used instead of putback_lru_pages(),
 * whenever the isolated pageset has been built by isolate_migratepages_range()
 */
void putback_movable_pages(struct list_head *l)
{
	struct page *page;
	struct page *page2;

	list_for_each_entry_safe(page, page2, l, lru) {
		list_del(&page->lru);
		dec_zone_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
		if (unlikely(balloon_page_movable(page)))
			balloon_page_putback(page);
		else
			putback_lru_page(page);
	}
}
#endif // xzl

/*
 * Restore a potential migration pte to a working pte entry
 */
static int remove_migration_pte(struct page *new, struct vm_area_struct *vma,
				 unsigned long addr, void *old)
{
	struct mm_struct *mm = vma->vm_mm;
	swp_entry_t entry;
 	pmd_t *pmd;
	pte_t *ptep, pte;
 	spinlock_t *ptl;

	if (unlikely(PageHuge(new))) {
		ptep = huge_pte_offset(mm, addr);
		if (!ptep)
			goto out;
		ptl = &mm->page_table_lock;
	} else {
		pmd = mm_find_pmd(mm, addr);
		if (!pmd)
			goto out;
		if (pmd_trans_huge(*pmd))
			goto out;

		ptep = pte_offset_map(pmd, addr);

		/*
		 * Peek to check is_swap_pte() before taking ptlock?  No, we
		 * can race mremap's move_ptes(), which skips anon_vma lock.
		 */

		ptl = pte_lockptr(mm, pmd);
	}

 	spin_lock(ptl);
	pte = *ptep;
	if (!is_swap_pte(pte))
		goto unlock;

	entry = pte_to_swp_entry(pte);

	if (!is_migration_entry(entry) ||
	    migration_entry_to_page(entry) != old)
		goto unlock;

	get_page(new);
	pte = pte_mkold(mk_pte(new, vma->vm_page_prot));
	if (is_write_migration_entry(entry))
		pte = pte_mkwrite(pte);
#ifdef CONFIG_HUGETLB_PAGE
	if (PageHuge(new)) {
		pte = pte_mkhuge(pte);
		pte = arch_make_huge_pte(pte, vma, new, 0);
	}
#endif
	flush_dcache_page(new);
	set_pte_at(mm, addr, ptep, pte);

	if (PageHuge(new)) {
		if (PageAnon(new))
			hugepage_add_anon_rmap(new, vma, addr);
		else
			page_dup_rmap(new);
	} else if (PageAnon(new))
		/* xzl: this increases page->_mapcount */
		page_add_anon_rmap(new, vma, addr);
	else
		page_add_file_rmap(new);

	/* No need to invalidate - it was non-present before */
	update_mmu_cache(vma, addr, ptep);
unlock:
	pte_unmap_unlock(ptep, ptl);
out:
	//check_vma(new);	// xzl: debugging
	return SWAP_AGAIN;
}

static int install_new_pte(struct page *new, struct vm_area_struct *vma,
				 unsigned long addr, void *pgdesc)
{
	struct mm_struct *mm = vma->vm_mm;
	struct mig_pg_desc *pg_desc = pgdesc;
 	pmd_t *pmd;
	pte_t *ptep, pte, oldpte;
 	spinlock_t *ptl;
 	struct page *page = pg_desc->page; /* old page to be replaced */

 	/* xzl: should we use page_check_address()? */

 	D("mm %pK page %pK addr %08lx", mm, page, addr);

	if (unlikely(PageHuge(new))) {
		ptep = huge_pte_offset(mm, addr);
		if (!ptep)
			goto out;
		//ptl = &mm->page_table_lock;
		ptl = huge_pte_lockptr(hstate_vma(vma), mm, ptep); //hym: modify 6.5
	} else {
		pmd = mm_find_pmd(mm, addr);
		if (!pmd)
			goto out;
		if (pmd_trans_huge(*pmd))
			goto out;

		ptep = pte_offset_map(pmd, addr);

		/*
		 * Peek to check is_swap_pte() before taking ptlock?  No, we
		 * can race mremap's move_ptes(), which skips anon_vma lock.
		 */

		ptl = pte_lockptr(mm, pmd);
	}

 	spin_lock(ptl);
 	oldpte = *ptep;

	get_page(new);

	/* xzl: we create the new pte as young */
	pte = pte_mkyoung(mk_pte(new, vma->vm_page_prot));
	if (pte_write(oldpte))
		pte = pte_mkwrite(pte);

	if (PageHuge(new)) {
		pte = pte_mkhuge(pte);
		pte = arch_make_huge_pte(pte, vma, new, 0);
	} //hym: add 6.5

	flush_dcache_page(new);
	set_pte_at(mm, addr, ptep, pte);

	/* Save it. we must do it after @set_pte_at(), which will synthesize
	 * some extra bits by itself (e.g., L_PTE_RDONLY, PTE_EXT_NG..)
	 * after migration, we expect @pte is still young if no race */
	pg_desc->ptep = ptep;
	pg_desc->pte = *ptep;

	if (PageHuge(new)) {
		if (PageAnon(new))
			hugepage_add_anon_rmap(new, vma, addr);
		else
			page_dup_rmap(new);
	} else if (PageAnon(new))
		/* xzl: this increases page->_mapcount */
		page_add_anon_rmap(new, vma, addr);
	else
		page_add_file_rmap(new);

	/* No need to invalidate - it was non-present before */
	update_mmu_cache(vma, addr, ptep);

	/* From try_to_unmap_one(). this decreases page->_mapcount */
	page_remove_rmap(page);
	page_cache_release(page); /* xzl: i.e. put_page() */

//unlock:
	pte_unmap_unlock(ptep, ptl);
out:
	//check_vma(new);	// xzl: debugging
	return SWAP_AGAIN;
}

/*
 * Get rid of all migration entries and replace them by
 * references to the indicated page.
 */
static void remove_migration_ptes(struct page *old, struct page *new)
{
	//rmap_walk(new, remove_migration_pte, old);

	/*********** modified by hym  *****************/
	struct rmap_walk_control rwc = {
		.rmap_one = remove_migration_pte,
		.arg = old,
	};

	rmap_walk(new, &rwc);
}

#if 0 // xzl, use migrate.c
/*
 * Something used the pte of a page under migration. We need to
 * get to the page and wait until migration is finished.
 * When we return from this function the fault will be retried.
 */
static void __migration_entry_wait(struct mm_struct *mm, pte_t *ptep,
				spinlock_t *ptl)
{
	pte_t pte;
	swp_entry_t entry;
	struct page *page;

	spin_lock(ptl); // xzl: the pgtable spinlock?
	pte = *ptep;
	if (!is_swap_pte(pte))
		goto out;

	entry = pte_to_swp_entry(pte);
	if (!is_migration_entry(entry))
		goto out;

	page = migration_entry_to_page(entry);

	/*
	 * Once radix-tree replacement of page migration started, page_count
	 * *must* be zero. And, we don't want to call wait_on_page_locked()
	 * against a page without get_page().
	 * So, we use get_page_unless_zero(), here. Even failed, page fault
	 * will occur again.
	 */
	if (!get_page_unless_zero(page))
		goto out;
	pte_unmap_unlock(ptep, ptl); //xzl: why unmap pte? (in case of high pte)
	//xzl: wait until the page is unlocked. by whom? move_to_new_page()?
	wait_on_page_locked(page);
	put_page(page);
	return;
out:
	pte_unmap_unlock(ptep, ptl);
}

void migration_entry_wait(struct mm_struct *mm, pmd_t *pmd,
				unsigned long address)
{
	spinlock_t *ptl = pte_lockptr(mm, pmd);
	pte_t *ptep = pte_offset_map(pmd, address);
	__migration_entry_wait(mm, ptep, ptl);
}

void migration_entry_wait_huge(struct mm_struct *mm, pte_t *pte)
{
	spinlock_t *ptl = &(mm)->page_table_lock;
	__migration_entry_wait(mm, pte, ptl);
}
#endif

#ifdef CONFIG_BLOCK
/* Returns true if all buffers are successfully locked */
static bool buffer_migrate_lock_buffers(struct buffer_head *head,
							enum migrate_mode mode)
{
	struct buffer_head *bh = head;

	/* Simple case, sync compaction */
	if (mode != MIGRATE_ASYNC) {
		do {
			get_bh(bh);
			lock_buffer(bh);
			bh = bh->b_this_page;

		} while (bh != head);

		return true;
	}

	/* async case, we cannot block on lock_buffer so use trylock_buffer */
	do {
		get_bh(bh);
		if (!trylock_buffer(bh)) {
			/*
			 * We failed to lock the buffer and cannot stall in
			 * async migration. Release the taken locks
			 */
			struct buffer_head *failed_bh = bh;
			put_bh(failed_bh);
			bh = head;
			while (bh != failed_bh) {
				unlock_buffer(bh);
				put_bh(bh);
				bh = bh->b_this_page;
			}
			return false;
		}

		bh = bh->b_this_page;
	} while (bh != head);
	return true;
}
#else
static inline bool buffer_migrate_lock_buffers(struct buffer_head *head,
							enum migrate_mode mode)
{
	return true;
}
#endif /* CONFIG_BLOCK */

/*
 * Replace the page in the mapping.
 *
 * The number of remaining references must be:
 * 1 for anonymous pages without a mapping
 * 2 for pages with a mapping
 * 3 for pages with a mapping and PagePrivate/PagePrivate2 set.
 */

int migrate_page_move_mapping(struct address_space *mapping,
                struct page *newpage, struct page *page,
                struct buffer_head *head, enum migrate_mode mode,
                int extra_count)
{
        struct zone *oldzone, *newzone;
        int dirty;
        int expected_count = 1 + extra_count;
        void **pslot;

        if (!mapping) {
                /* Anonymous page without mapping */
                if (page_count(page) != expected_count)
                        return -EAGAIN;

                /* No turning back from here */
                set_page_memcg(newpage, page_memcg(page));
                newpage->index = page->index;
                newpage->mapping = page->mapping;
                if (PageSwapBacked(page))
                        SetPageSwapBacked(newpage);

                return MIGRATEPAGE_SUCCESS;
        }

        oldzone = page_zone(page);
        newzone = page_zone(newpage);

        spin_lock_irq(&mapping->tree_lock);

        pslot = radix_tree_lookup_slot(&mapping->page_tree,
                                        page_index(page));

        expected_count += 1 + page_has_private(page);
        if (page_count(page) != expected_count ||
                radix_tree_deref_slot_protected(pslot, &mapping->tree_lock) != page) {
                spin_unlock_irq(&mapping->tree_lock);
                return -EAGAIN;
        }

        if (!page_freeze_refs(page, expected_count)) {
                spin_unlock_irq(&mapping->tree_lock);
                return -EAGAIN;
        }
	
	/*
         * In the async migration case of moving a page with buffers, lock the
         * buffers using trylock before the mapping is moved. If the mapping
         * was moved, we later failed to lock the buffers and could not move
         * the mapping back due to an elevated page count, we would have to
         * block waiting on other references to be dropped.
         */
        if (mode == MIGRATE_ASYNC && head &&
                        !buffer_migrate_lock_buffers(head, mode)) {
                page_unfreeze_refs(page, expected_count);
                spin_unlock_irq(&mapping->tree_lock);
                return -EAGAIN;
        }

        /*
         * Now we know that no one else is looking at the page:
         * no turning back from here.
         */
        set_page_memcg(newpage, page_memcg(page));
        newpage->index = page->index;
        newpage->mapping = page->mapping;
        if (PageSwapBacked(page))
                SetPageSwapBacked(newpage);

        get_page(newpage);      /* add cache reference */
        if (PageSwapCache(page)) {
                SetPageSwapCache(newpage);
                set_page_private(newpage, page_private(page));
        }

        /* Move dirty while page refs frozen and newpage not yet exposed */
        dirty = PageDirty(page);
        if (dirty) {
                ClearPageDirty(page);
                SetPageDirty(newpage);
        }

        radix_tree_replace_slot(pslot, newpage);

        /*
         * Drop cache reference from old page by unfreezing
         * to one less reference.
         * We know this isn't the last reference.
         */
        page_unfreeze_refs(page, expected_count - 1);

        spin_unlock(&mapping->tree_lock);
        /* Leave irq disabled to prevent preemption while updating stats */

        /*
         * If moved to a different zone then also account
         * the page for that zone. Other VM counters will be
         * taken care of when we establish references to the
         * new page and drop references to the old page.
         *
         * Note that anonymous pages are accounted for
         * via NR_FILE_PAGES and NR_ANON_PAGES if they
         * are mapped to swap space.
         */
        if (newzone != oldzone) {
                __dec_zone_state(oldzone, NR_FILE_PAGES);
                __inc_zone_state(newzone, NR_FILE_PAGES);
                if (PageSwapBacked(page) && !PageSwapCache(page)) {
                        __dec_zone_state(oldzone, NR_SHMEM);
                        __inc_zone_state(newzone, NR_SHMEM);
                }
                if (dirty && mapping_cap_account_dirty(mapping)) {
                        __dec_zone_state(oldzone, NR_FILE_DIRTY);
                        __inc_zone_state(newzone, NR_FILE_DIRTY);
                }
        }
        local_irq_enable();

        return MIGRATEPAGE_SUCCESS;
}

	

#if 0
static int migrate_page_move_mapping(struct address_space *mapping,
		struct page *newpage, otruct page *page,
		struct buffer_head *head, enum migrate_mode mode)
{
	int expected_count = 0;
	//int expected_count = 1 + extra_count; //hym
	
	void **pslot;

	if (!mapping) {
		/* Anonymous page without mapping */
		if (page_count(page) != 1)
			return -EAGAIN;
		D("xzl:%s:anon pg no mapping. done\n", __func__);
		return MIGRATEPAGE_SUCCESS;
	}

	spin_lock_irq(&mapping->tree_lock);

	pslot = radix_tree_lookup_slot(&mapping->page_tree,
 					page_index(page));

	expected_count = 2 + page_has_private(page);
	if (page_count(page) != expected_count ||
		radix_tree_deref_slot_protected(pslot, &mapping->tree_lock) != page) {
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	if (!page_freeze_refs(page, expected_count)) {
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	/*
	 * In the async migration case of moving a page with buffers, lock the
	 * buffers using trylock before the mapping is moved. If the mapping
	 * was moved, we later failed to lock the buffers and could not move
	 * the mapping back due to an elevated page count, we would have to
	 * block waiting on other references to be dropped.
	 *
	 * xzl: does this mean moving a page used as buffer cache?
	 */
	if (mode == MIGRATE_ASYNC && head &&
			!buffer_migrate_lock_buffers(head, mode)) {
		page_unfreeze_refs(page, expected_count);
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	/*
	 * Now we know that no one else is looking at the page.
	 */
	get_page(newpage);	/* add cache reference */
	if (PageSwapCache(page)) {
		SetPageSwapCache(newpage);
		set_page_private(newpage, page_private(page));
	}

	radix_tree_replace_slot(pslot, newpage);

	/*
	 * Drop cache reference from old page by unfreezing
	 * to one less reference.
	 * We know this isn't the last reference.
	 */
	page_unfreeze_refs(page, expected_count - 1);

	/*
	 * If moved to a different zone then also account
	 * the page for that zone. Other VM counters will be
	 * taken care of when we establish references to the
	 * new page and drop references to the old page.
	 *
	 * Note that anonymous pages are accounted for
	 * via NR_FILE_PAGES and NR_ANON_PAGES if they
	 * are mapped to swap space.
	 */
	__dec_zone_page_state(page, NR_FILE_PAGES);
	__inc_zone_page_state(newpage, NR_FILE_PAGES);
	if (!PageSwapCache(page) && PageSwapBacked(page)) {
		__dec_zone_page_state(page, NR_SHMEM);
		__inc_zone_page_state(newpage, NR_SHMEM);
	}
	spin_unlock_irq(&mapping->tree_lock);

	return MIGRATEPAGE_SUCCESS;
}
#endif

#if 0 // xzl, --> migrate.c
/*
 * The expected number of remaining references is the same as that
 * of migrate_page_move_mapping().
 *
 * xzl: only matters when @mapping exists (i.e. not for anon mapping)
 */
int migrate_huge_page_move_mapping(struct address_space *mapping,
				   struct page *newpage, struct page *page)
{
	int expected_count;
	void **pslot;

	if (!mapping) {
		if (page_count(page) != 1)
			return -EAGAIN;
		return MIGRATEPAGE_SUCCESS;
	}

	spin_lock_irq(&mapping->tree_lock);

	pslot = radix_tree_lookup_slot(&mapping->page_tree,
					page_index(page));

	expected_count = 2 + page_has_private(page);
	if (page_count(page) != expected_count ||
		radix_tree_deref_slot_protected(pslot, &mapping->tree_lock) != page) {
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	if (!page_freeze_refs(page, expected_count)) {
		spin_unlock_irq(&mapping->tree_lock);
		return -EAGAIN;
	}

	get_page(newpage);

	radix_tree_replace_slot(pslot, newpage);

	page_unfreeze_refs(page, expected_count - 1);

	spin_unlock_irq(&mapping->tree_lock);
	return MIGRATEPAGE_SUCCESS;
}
#endif

static void copy_page_dma_sync(struct page *newpage, struct page *page)
{
	dma_cookie_t c;
	if (!chans[0])
		chans[0] = init_dma_chan();

	c = dma_async_memcpy_pg_to_pg(chans[0], newpage, 0, page, 0, PAGE_SIZE);
	pr_info("wait for memcpy to complete...");

	while (dma_async_is_tx_complete(chans[0], c, NULL, NULL) == DMA_IN_PROGRESS)
	{
	   dma_sync_wait(chans[0], c);	// this will time out
	}
	pr_info("done\n");
}

#if 0
static void copy_page_dma_async(struct page *newpage, struct page *page,
		struct mig_pg_desc *desc)
{
	dma_cookie_t c;
	struct dma_async_tx_descriptor *tx = NULL;
	unsigned long flags;

	if (!chan)
		chan = init_dma_chan();

	/* XXX: submit the xfer desc to the dma engine...
	 * when to call issue()?
	 */
}
#endif


/********************* copyied from x86's migrate.c hym *******************************/

/*
 * Gigantic pages are so large that we do not guarantee that page++ pointer
 * arithmetic will work across the entire page.  We need something more
 * specialized.
 */
static void __copy_gigantic_page(struct page *dst, struct page *src,
                                int nr_pages)
{
        int i;
        struct page *dst_base = dst;
        struct page *src_base = src;

        for (i = 0; i < nr_pages; ) {
                cond_resched();
                copy_highpage(dst, src);

                i++;
                dst = mem_map_next(dst, dst_base, i);
                src = mem_map_next(src, src_base, i);
        }
}


static void copy_huge_page(struct page *dst, struct page *src)
{
        int i;
        int nr_pages;

        if (PageHuge(src)) {
                /* hugetlbfs page */
                struct hstate *h = page_hstate(src);
                nr_pages = pages_per_huge_page(h);

                if (unlikely(nr_pages > MAX_ORDER_NR_PAGES)) {
                        __copy_gigantic_page(dst, src, nr_pages);
                        return;
                }
        } else {
                /* thp page */
                BUG_ON(!PageTransHuge(src));
                nr_pages = hpage_nr_pages(src);
        }

        for (i = 0; i < nr_pages; i++) {
                cond_resched();
                copy_highpage(dst + i, src + i);
        }
}

/******************* end copy from migrate.c hym *********************/

/*
 * Copy the page to its new location
 *
 * xzl: XXX split the func into two.
 * start dma and return the status like "ongoing"
 *
 * passing mig_desc* down to the dma func,
 * so that in the dma completion callback, the 2nd half of this func can be
 * executed and the desc can thus be marked.
 *
 * @usedma:
 * 0 vanilla memcpy
 * 1 dma sync
 * 2 dma async
 */
int migrate_page_copy_ks2(struct page *newpage, struct page *page,
		struct mig_pg_desc *desc, int usedma)
{
	int rc;

	D("userdma = %d", usedma);

	if (PageHuge(page) || PageTransHuge(page))
		copy_huge_page(newpage, page);
	else {
		if (usedma == 1) {
			copy_page_dma_sync(newpage, page);
			rc = MIGRATEPAGE_SUCCESS;
		}
		else if (usedma == 2) {
			return MIGRATEPAGE_PENDING; /* do nothing */
		}
		else {	/* vanilla copy */
			copy_highpage(newpage, page);
			rc = MIGRATEPAGE_SUCCESS;
		}
	}

	if (PageError(page))
		SetPageError(newpage);
	if (PageReferenced(page))
		SetPageReferenced(newpage);
	if (PageUptodate(page))
		SetPageUptodate(newpage);
	if (TestClearPageActive(page)) {
		VM_BUG_ON(PageUnevictable(page));
		SetPageActive(newpage);
	} else if (TestClearPageUnevictable(page))
		SetPageUnevictable(newpage);
	if (PageChecked(page))
		SetPageChecked(newpage);
	if (PageMappedToDisk(page))
		SetPageMappedToDisk(newpage);

	if (PageDirty(page)) {
		clear_page_dirty_for_io(page);
		/*
		 * Want to mark the page and the radix tree as dirty, and
		 * redo the accounting that clear_page_dirty_for_io undid,
		 * but we can't use set_page_dirty because that function
		 * is actually a signal that all of the page has become dirty.
		 * Whereas only part of our page may be dirty.
		 */
		if (PageSwapBacked(page))
			SetPageDirty(newpage);
		else
			__set_page_dirty_nobuffers(newpage);
 	}

	/* xzl: this migrates page's mlock flags? */
	mlock_migrate_page(newpage, page);
	ksm_migrate_page(newpage, page);
	/*
	 * Please do not reorder this without considering how mm/ksm.c's
	 * get_ksm_page() depends upon ksm_migrate_page() and PageSwapCache().
	 */
	ClearPageSwapCache(page);
	ClearPagePrivate(page);
	set_page_private(page, 0);

	/*
	 * If any waiters have accumulated on the new page then
	 * wake them up.
	 * xzl: meaning someone wants to writeback page during migration? so
	 * let them proceed now?
	 * note: the new ptes are not up yet
	 */
	if (PageWriteback(newpage))
		end_page_writeback(newpage);

	return rc;
}

#if 0 /* xzl */
/*
 * Copy the page to its new location
 */
void migrate_page_copy(struct page *newpage, struct page *page)
{
	if (PageHuge(page) || PageTransHuge(page))
		copy_huge_page(newpage, page);
	else
		copy_highpage(newpage, page);

	if (PageError(page))
		SetPageError(newpage);
	if (PageReferenced(page))
		SetPageReferenced(newpage);
	if (PageUptodate(page))
		SetPageUptodate(newpage);
	if (TestClearPageActive(page)) {
		VM_BUG_ON(PageUnevictable(page));
		SetPageActive(newpage);
	} else if (TestClearPageUnevictable(page))
		SetPageUnevictable(newpage);
	if (PageChecked(page))
		SetPageChecked(newpage);
	if (PageMappedToDisk(page))
		SetPageMappedToDisk(newpage);

	if (PageDirty(page)) {
		clear_page_dirty_for_io(page);
		/*
		 * Want to mark the page and the radix tree as dirty, and
		 * redo the accounting that clear_page_dirty_for_io undid,
		 * but we can't use set_page_dirty because that function
		 * is actually a signal that all of the page has become dirty.
		 * Whereas only part of our page may be dirty.
		 */
		if (PageSwapBacked(page))
			SetPageDirty(newpage);
		else
			__set_page_dirty_nobuffers(newpage);
 	}

	/* xzl: this migrates page's mlock flags? */
	mlock_migrate_page(newpage, page);
	ksm_migrate_page(newpage, page);
	/*
	 * Please do not reorder this without considering how mm/ksm.c's
	 * get_ksm_page() depends upon ksm_migrate_page() and PageSwapCache().
	 */
	ClearPageSwapCache(page);
	ClearPagePrivate(page);
	set_page_private(page, 0);

	/*
	 * If any waiters have accumulated on the new page then
	 * wake them up.
	 * xzl: meaning someone wants to writeback page during migration? so
	 * let them proceed now?
	 * note: the new ptes are not up yet
	 */
	if (PageWriteback(newpage))
		end_page_writeback(newpage);
}
#endif

/************************************************************
 *                    Migration functions
 ***********************************************************/

#if 0
/* Always fail migration. Used for mappings that are not movable */
int fail_migrate_page(struct address_space *mapping,
			struct page *newpage, struct page *page)
{
	return -EIO;
}
EXPORT_SYMBOL(fail_migrate_page);
#endif

/*
 * Common logic to directly migrate a single page suitable for
 * pages that do not use PagePrivate/PagePrivate2.
 *
 * Pages are locked upon entry and exit.
 */
static int migrate_page_ks2(struct address_space *mapping,
		struct page *newpage, struct page *page,
		enum migrate_mode mode, struct mig_pg_desc *desc)
{
	int rc;

	BUG_ON(PageWriteback(page));	/* Writeback must be complete */

	// xzl: replaces the page in the address_space
	//rc = migrate_page_move_mapping(mapping, newpage, page, NULL, mode);
	rc = migrate_page_move_mapping(mapping, newpage, page, NULL, mode, 0);

	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	// xzl: @mode does not matter for the actual copy
	// XXX: change this to switch the copymode
	return migrate_page_copy_ks2(newpage, page, desc, 2);
//	return MIGRATEPAGE_SUCCESS;
}

//#ifdef CONFIG_BLOCK
#if 0	// xzl
/*
 * Migration function for pages with buffers. This function can only be used
 * if the underlying filesystem guarantees that no other references to "page"
 * exist.
 */
int buffer_migrate_page(struct address_space *mapping,
		struct page *newpage, struct page *page, enum migrate_mode mode)
{
	struct buffer_head *bh, *head;
	int rc;

	if (!page_has_buffers(page))
		return migrate_page(mapping, newpage, page, mode);

	head = page_buffers(page);

	rc = migrate_page_move_mapping(mapping, newpage, page, head, mode);

	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	/*
	 * In the async case, migrate_page_move_mapping locked the buffers
	 * with an IRQ-safe spinlock held. In the sync case, the buffers
	 * need to be locked now
	 */
	if (mode != MIGRATE_ASYNC)
		BUG_ON(!buffer_migrate_lock_buffers(head, mode));

	ClearPagePrivate(page);
	set_page_private(newpage, page_private(page));
	set_page_private(page, 0);
	put_page(page);
	get_page(newpage);

	bh = head;
	do {
		set_bh_page(bh, newpage, bh_offset(bh));
		bh = bh->b_this_page;

	} while (bh != head);

	SetPagePrivate(newpage);

	migrate_page_copy(newpage, page);

	bh = head;
	do {
		unlock_buffer(bh);
 		put_bh(bh);
		bh = bh->b_this_page;

	} while (bh != head);

	return MIGRATEPAGE_SUCCESS;
}
EXPORT_SYMBOL(buffer_migrate_page);
#endif

/*
 * Writeback a page to clean the dirty state
 */
static int writeout(struct address_space *mapping, struct page *page)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_NONE,
		.nr_to_write = 1,
		.range_start = 0,
		.range_end = LLONG_MAX,
		.for_reclaim = 1
	};
	int rc;

	if (!mapping->a_ops->writepage)
		/* No write method for the address space */
		return -EINVAL;

	if (!clear_page_dirty_for_io(page))
		/* Someone else already triggered a write */
		return -EAGAIN;

	/*
	 * A dirty page may imply that the underlying filesystem has
	 * the page on some queue. So the page must be clean for
	 * migration. Writeout may mean we loose the lock and the
	 * page state is no longer what we checked for earlier.
	 * At this point we know that the migration attempt cannot
	 * be successful.
	 */
	remove_migration_ptes(page, page);

	rc = mapping->a_ops->writepage(page, &wbc);

	if (rc != AOP_WRITEPAGE_ACTIVATE)
		/* unlocked. Relock */
		lock_page(page);

	return (rc < 0) ? -EIO : -EAGAIN;
}

/*
 * Default handling if a filesystem does not provide a migration function.
 */
static int fallback_migrate_page(struct address_space *mapping,
	struct page *newpage, struct page *page, enum migrate_mode mode,
	struct mig_pg_desc *desc)
{
	// xzl: PG_dirty means forcing writing to disk? who will request this?
	if (PageDirty(page)) {
		/* Only writeback pages in full synchronous migration */
		if (mode != MIGRATE_SYNC)
			return -EBUSY;
		return writeout(mapping, page);
	}

	/*
	 * Buffers may be managed in a filesystem specific way.
	 * We must have no buffers or drop them.
	 */
	if (page_has_private(page) &&
	    !try_to_release_page(page, GFP_KERNEL))
		return -EAGAIN;

	return migrate_page_ks2(mapping, newpage, page, mode, desc);
}

/*
 * Move a page to a newly allocated page
 * The page is locked and all ptes have been successfully removed.
 *
 * The new page will have replaced the old page if this function
 * is successful.
 *
 * Return value:
 *   < 0 - error code
 *  MIGRATEPAGE_SUCCESS - success
 *
 *  xzl: @remap_swapcache seems a little misnomer:
 *  0 seems to mean that this is a swapcache page and shouldn't be mapped after
 *  migration.
 *  1 seem to mean it should be mapped (all other cases)
 */
static int move_to_new_page_ks2(struct page *newpage, struct page *page,
				int remap_swapcache, enum migrate_mode mode,
				struct mig_pg_desc *desc)
{
	struct address_space *mapping;
	int rc;

	/*
	 * Block others from accessing the page when we get around to
	 * establishing additional references. We are the only one
	 * holding a reference to the new page at this point.
	 */
	if (!trylock_page(newpage))
		BUG();

	/* Prepare mapping for the new page.*/
	newpage->index = page->index;
	newpage->mapping = page->mapping;
	if (PageSwapBacked(page))
		SetPageSwapBacked(newpage);

	// xzl: calls address_space's migratepage or the fallback one
	D("xzl:migrate page... pagecount %d", page_count(newpage));
	mapping = page_mapping(page);
	if (!mapping)
		rc = migrate_page_ks2(mapping, newpage, page, mode, desc);
	else if (mapping->a_ops->migratepage)
		/*
		 * Most pages have a mapping and most filesystems provide a
		 * migratepage callback. Anonymous pages are part of swap
		 * space which also has its own migratepage callback. This
		 * is the most common path for page migration.
		 */
		rc = mapping->a_ops->migratepage(mapping,
						newpage, page, mode);
	else
		rc = fallback_migrate_page(mapping, newpage, page, mode, desc);

	if (rc == MIGRATEPAGE_PENDING)
		return rc;

	// xzl: clear the mappings of old/new page depending on the outcome
	// of migration
	if (rc != MIGRATEPAGE_SUCCESS) {
		newpage->mapping = NULL;
	} else { // xzl: migration succeeds
		if (remap_swapcache)
			remove_migration_ptes(page, newpage);
		page->mapping = NULL;
	}

	unlock_page(newpage);

	return rc;
}

static int __unmap_and_move_ks2(struct page *page, struct page *newpage,
				int force, enum migrate_mode mode, struct mig_pg_desc *desc)
{
	int rc = -EAGAIN;
	int remap_swapcache = 1;
	struct mem_cgroup *mem;
	struct anon_vma *anon_vma = NULL;

	// xzl: lock page so it does not get paged out?
	if (!trylock_page(page)) {
		// xzl: t&s the "locked" bit. ret (!old value). return 1 means locks
		// well.
		if (!force || mode == MIGRATE_ASYNC)
			goto out;

		/*
		 * It's not safe for direct compaction to call lock_page.
		 * For example, during page readahead pages are added locked
		 * to the LRU. Later, when the IO completes the pages are
		 * marked uptodate and unlocked. However, the queueing
		 * could be merging multiple pages for one bio (e.g.
		 * mpage_readpages). If an allocation happens for the
		 * second or third page, the process can end up locking
		 * the same page twice and deadlocking. Rather than
		 * trying to be clever about what pages can be locked,
		 * avoid the use of lock_page for direct compaction
		 * altogether.
		 */
		if (current->flags & PF_MEMALLOC)
			goto out;

		lock_page(page);
	}

	/* charge against new page */
/*	// there is no cgroup in linux 4.4, so I comment these codes
	mem_cgroup_prepare_migration(page, newpage, &mem);
*/
	// xzl: we have locked the page.
	// so handling a bunch of tricky cases where unmapping should be careful.
	// check page flag -- is it being written back (to disk)?
	if (PageWriteback(page)) {
		/*
		 * Only in the case of a full synchronous migration is it
		 * necessary to wait for PageWriteback. In the async case,
		 * the retry loop is too short and in the sync-light case,
		 * the overhead of stalling is too much
		 */
		if (mode != MIGRATE_SYNC) {
			rc = -EBUSY;
			goto uncharge;
		}
		if (!force)
			goto uncharge;
		wait_on_page_writeback(page);
	}
	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 */
	// xzl: anon page -- trying to grab its anon_vma for rmap
	if (PageAnon(page) && !PageKsm(page)) {
		/*
		 * Only page_lock_anon_vma_read() understands the subtleties of
		 * getting a hold on an anon_vma from outside one of its mms.
		 */
		anon_vma = page_get_anon_vma(page);
		if (anon_vma) { //xzl: good -- got the anon_vma for the anon page
			/*
			 * Anon page
			 */
		} else if (PageSwapCache(page)) { //xzl: no anon_vma, page being swapped out...
			/*
			 * We cannot be sure that the anon_vma of an unmapped
			 * swapcache page is safe to use because we don't
			 * know in advance if the VMA that this page belonged
			 * to still exists. If the VMA and others sharing the
			 * data have been freed, then the anon_vma could
			 * already be invalid.
			 *
			 * To avoid this possibility, swapcache pages get
			 * migrated but are not remapped when migration
			 * completes
			 */
			remap_swapcache = 0;
		} else {
			goto uncharge;
		}
	}

	if (unlikely(balloon_page_movable(page))) {
		/*
		 * A ballooned page does not need any special attention from
		 * physical to virtual reverse mapping procedures.
		 * Skip any attempt to unmap PTEs or to remap swap cache,
		 * in order to avoid burning cycles at rmap level, and perform
		 * the page migration right away (proteced by page lock).
		 */
		rc = balloon_page_migrate(newpage, page, mode);
		goto uncharge;
	}

	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_complete_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 *
	 * xzl: why a bug if no mapping for an anon page?
	 */
	if (!page->mapping) {
		VM_BUG_ON(PageAnon(page));
		if (page_has_private(page)) {
			try_to_free_buffers(page);
			goto uncharge;
		}
		goto skip_unmap;
	}

	/* Establish migration ptes or remove ptes */
	// xzl: goes to rmap. these flag matters for the type of temporary mapping
	// this seems to do heavy-lifting
	// TTU_IGNORE_FLUSH_CACHE is add by hym at include/linux/rmap.h
	try_to_unmap(page, TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS|TTU_IGNORE_FLUSH_CACHE);

skip_unmap:
	if (!page_mapped(page))	{
		// xzl: unmap succeeds -- page mapped in no pagetable
		// now we have almost everything needed to fill @desc.
		// note that ->mm has already been set.
		desc->page = page;
		desc->newpage = newpage;
		desc->anon_vma = anon_vma;
		desc->remap_swapcache = remap_swapcache;
		rc = move_to_new_page_ks2(newpage, page, remap_swapcache, mode, desc);
	}

	if (rc == MIGRATEPAGE_PENDING)
		return rc;

	if (rc && remap_swapcache) // xzl: if failed, restore original mapping
		remove_migration_ptes(page, page);

	/* Drop an anon_vma reference if we took one */
	// xzl: can drop the ref only after remove_migration_ptes which needs rmap
	if (anon_vma)
		put_anon_vma(anon_vma);

uncharge:
/*      
	//hym: there is no cgroup in linux 4.4, so I comment these codes, right???
	mem_cgroup_end_migration(mem, page, newpage,
				 (rc == MIGRATEPAGE_SUCCESS ||
				  rc == MIGRATEPAGE_BALLOON_SUCCESS));
*/
	unlock_page(page);
out:
	return rc;
}

//static struct page *lastpage = NULL;		// debugging

static int dma_complete_one(struct mig_pg_desc *desc)
{
	struct page *page = desc->page;
	struct page *newpage = desc->newpage;
	int remap_swapcache = desc->remap_swapcache;
	struct anon_vma *anon_vma = desc->anon_vma;
	struct mem_cgroup * mem = desc->mem;

	D("enter. page count %d", page_count(newpage));

	/*
	 * from migrate_page_copy()
	 * */
	if (PageError(page))
		SetPageError(newpage);
	if (PageReferenced(page))
		SetPageReferenced(newpage);
	if (PageUptodate(page))
		SetPageUptodate(newpage);
	if (TestClearPageActive(page)) {
		VM_BUG_ON(PageUnevictable(page));
		SetPageActive(newpage);
	} else if (TestClearPageUnevictable(page))
		SetPageUnevictable(newpage);
	if (PageChecked(page))
		SetPageChecked(newpage);
	if (PageMappedToDisk(page))
		SetPageMappedToDisk(newpage);

	if (PageDirty(page)) {
		clear_page_dirty_for_io(page);
		/*
		 * Want to mark the page and the radix tree as dirty, and
		 * redo the accounting that clear_page_dirty_for_io undid,
		 * but we can't use set_page_dirty because that function
		 * is actually a signal that all of the page has become dirty.
		 * Whereas only part of our page may be dirty.
		 */
		if (PageSwapBacked(page))
			SetPageDirty(newpage);
		else
			__set_page_dirty_nobuffers(newpage);
	}

	/* xzl: this migrates page's mlock flags? */
	mlock_migrate_page(newpage, page);
	ksm_migrate_page(newpage, page);
	/*
	 * Please do not reorder this without considering how mm/ksm.c's
	 * get_ksm_page() depends upon ksm_migrate_page() and PageSwapCache().
	 */
	ClearPageSwapCache(page);
	ClearPagePrivate(page);
	set_page_private(page, 0);

	/*
	 * If any waiters have accumulated on the new page then
	 * wake them up.
	 * xzl: meaning someone wants to writeback page during migration? so
	 * let them proceed now?
	 * note: the new ptes are not up yet
	 */
	if (PageWriteback(newpage))
		end_page_writeback(newpage);

	/*
	 * from move_to_new_page()
	 * */
	if (remap_swapcache)
		remove_migration_ptes(page, newpage);
	page->mapping = NULL;
	unlock_page(newpage);

	/*
	 * from __unmap_and_move()
	 * */
#if 0
	if (remap_swapcache) // xzl: if failed, restore original mapping
		remove_migration_ptes(page, page);
#endif

	/* Drop an anon_vma reference if we took one */
	// xzl: can drop the ref only after remove_migration_ptes which needs rmap
	if (anon_vma)
		put_anon_vma(anon_vma);
/*	// hym: cgroup is not used in linux 4.4, so I comment it 
	mem_cgroup_end_migration(mem, page, newpage, true);
*/
	unlock_page(page);

	/*
	  	from unmap_and_move()
	 	moves the old page from isolated_anon / isolated_file
		list to where it belongs to.
	 */
	list_del(&page->lru);
	dec_zone_page_state(page, NR_ISOLATED_ANON +
			page_is_file_cache(page));
	putback_lru_page(page);
	V("oldpage back to lru. count %d ", page_count(page));

	/*
	 * Move the new page to the LRU. If migration was not successful
	 * then this will free the page.
	 *
	 * xzl: note pages are first staged in a per-cpu per-lru pagevec then get
	 * moved to lru in batches. This has two implications:
	 * 1. PageLRU(page) may equal 0 after this call.
	 * 2. refcnt -1 for removing from isolated LRU;
	 * 	  refcnt + 1 for adding to pagevec by page_cache_get().
	 *    when removed from pagevec to LRU,  __pagevec_lru_add() will
	 * 	  call release_pages() to dec refcnt.
	 */
	D("before putback: count %d", page_count(newpage));

	putback_lru_page(newpage);
	D("newpage %pK back in lru. PageLRU %d PageUnevictable %d "
			"Active %d count %d mapcount %d",
			newpage,
			PageLRU(newpage), PageUnevictable(newpage), PageActive(newpage),
			page_count(newpage), page_mapcount(newpage));

//	lastpage = newpage; // debugging -- to check if the newpage lives later

	/* xzl: XXX change the status in desc->mig and move the desc
	 * to the completion queue. */
	return MIGRATEPAGE_SUCCESS;
}

static void do_dma_complete(struct mig_table *table)
{
	mig_region *r = table->region;
	int i, rc;
	mig_desc *desc = NULL;
	color_t clr;

	W("callback is invoked. tx %08x", table->tx_id);

	k2_measure("cb:start");

	down_read(&table->mm->mmap_sem);

	for (i = 0; i < MAX_NR_SG; i++) {
		if (last_pg_desc(table->pg_desc[i]))
			break; 	// marked end
		rc = dma_complete_one(table->pg_desc + i);
	}

	up_read(&table->mm->mmap_sem);

	BUG_ON(!r);

	k2_measure("cb:remap-end");

	/* The completed mig descs should be the ones on the head of issued queue.
	 * as all mig descs in a region are processed FIFO.
	 *
	 * Due to queue coloring,
	 * user and kernel will never concurrently flush request queue into
	 * the issued queue, resulting two sglists interleaved on the issued queue */

	for (i = 0; i < table->num_mig_descs; i++) {
		desc = mig_dequeue(r, &(r->qissued));
		BUG_ON(!desc);
		mark_mig_desc_comp(desc);
		mig_enqueue(r, &(r->qcomp), desc);
	}

	k2_measure("cb:enqueue-end");

	/* XXX notify the epoll() user? */

	/* examine the request queue (XXX examine all req queues) */
	clr = migqueue_color(r, &(r->qreq));
	if (clr == COLOR_USR) {
		/* user is supposed to flush the queue in the future. skip */
		;
	} else if (clr == COLOR_KER) {
		/* kernel (tasklet) is supposed to flush.
		 * no race condition exists:
		 * - user won't flush (thus change the color) of a COLOR_KER queue
		 * - tasklet is serialized by kernel design.
		 */
		assert(table->mm);
		flush_single_qreq(r, 0, table->mm, table->tx_id);
	}

	/*
	 * from migrate_pages()
	 * allocated by flush_single_qreq() as a whole page.
	 */
//	kfree(table);
	free_pages((unsigned long)table, MIGTABLE_PAGE_ORDER);
	k2_measure("cb:end");
}

/* work queue wrapper */
static void dma_complete_work(struct work_struct *work)
{
	struct mig_table *table = container_of(work, struct mig_table, work);
	do_dma_complete(table);
}

/* in softirq context */
static void dma_callback(void *param)
{
	struct mig_table *table = (struct mig_table *)param; /* an desc array */

//	return;		// this should make the user process block forever.
	k2_measure("softirq");

#if 1
	/* do it in a kernel thread */
	INIT_WORK(&(table->work), dma_complete_work);
	schedule_work(&(table->work));
#else
	/* call directly in softirq context. */
	do_dma_complete(table);
#endif
}

/* in a kernel thread -- should be fairly lightweight (for replication) */
static void dma_complete_work_noremap(struct work_struct *work)
{
	struct mig_table *table = container_of(work, struct mig_table, work);
	color_t clr;
	mig_region *r = table->region;

	BUG_ON(!table);

	/* examine the request queue (XXX examine all req queues) */
	clr = migqueue_color(r, &(r->qreq));
	if (clr == COLOR_USR) {
		/* user is supposed to flush the queue in the future. skip */
		;
	} else if (clr == COLOR_KER) {
		/* kernel (tasklet) is supposed to flush.
		 * no race condition exists:
		 * - user won't flush (thus change the color) of a COLOR_KER queue
		 * - tasklet is serialized by kernel design.
		 */
		assert(table->mm);
		flush_single_qreq_noremap2(r, 0, table->mm, table->tx_id);
	}

	/*
	 * from migrate_pages()
	 * allocated by flush_single_qreq() as a whole page.
	 */
	free_pages((unsigned long)table, MIGTABLE_PAGE_ORDER);
	k2_measure("cb:end");
}

/* in softirq */
static void dma_callback_noremap(void *param)
{
	struct mig_table *table = (struct mig_table *)param;
	mig_region *r = table->region;
	struct mmap_info *info = (struct mmap_info *) \
			((unsigned long)r + PAGE_SIZE * (NPAGES-1));
	mig_desc *desc = NULL;

	k2_measure("softirq/nr");

	W("callback is invoked. tx %08x (npages %d in %d descs)",
			table->tx_id, table->num_pages, table->num_mig_descs);

	BUG_ON(!r);

	/* the completed mig descs should be the ones on the head of issued queue.
	 * as all mig descs in a region are processd FIFO.
	 *
	 * Due to queue coloring,
	 * user and kernel will never concurrently flush request queue into
	 * the issued queue, resulting two sglists interleaved on the issued queue
	 *
	 * The following takes ~2us for one desc.. why?
	 * */

	while ((desc = mig_dequeue(r, &(r->qissued)))) {
		mark_mig_desc_comp(desc);
		mig_enqueue(r, &(r->qcomp), desc);
	}

	k2_measure("cb:enqueue-end/nr");
//	k2_flush_measure_format();

    /* notify the epoll() waiters.
	 *
     * Are we too aggressive (e.g. Should we defer the wakeup to kthread)?
     * probably not. After all, the overhead should be small if no
     * user is waiting. And the users should be cautious in
     * calling e/poll().
     *
     * This is likely to wake up users earlier by ~5-6 us, saving a latency
     * of switch-to-kthread. */
	wake_up_interruptible(&info->wait_queue);

	/* mm refcnt has to be dec by the following */
	INIT_WORK(&(table->work), dma_complete_work_noremap);
	schedule_work(&(table->work));
//	schedule_work_on(2, &(table->work));
}

/* can be used in softirq */
static int dma_complete_one_mig(struct mig_pg_desc *desc)
{
	struct page *page = desc->page;
	struct page *newpage = desc->newpage;
	struct mem_cgroup * mem = desc->mem;
	pte_t pte;

	V("enter. newpage count %d *ptep %llx pte %llx",
			page_count(newpage), *(desc->ptep), desc->pte);

	/* install_new_pte() installed a young pte.
	 * anybody has touched the page? if not, replace it with an old pte
	 * so that no page fault will result.
	 * (the assumption is that the pte has not become old and changed
	 * back to young again). */
	pte = pte_mkold(desc->pte);
	
	//printk("hym: func %s, line %d\n", __func__, __LINE__);

#if 1 /* perhaps we should only replace the lower word of pte */
	
	/************************************


	why error: incompatible type for argument 1 of ‘__sync_bool_compare_and_swap’
	
	*/
	//if (!__sync_bool_compare_and_swap(desc->ptep,desc->pte, pte)) { //wrong
	if (!__sync_bool_compare_and_swap((long long *)desc->ptep,*(long long *)&desc->pte, *(long long *)&pte)) {
	//if (!__sync_bool_compare_and_swap((long long *)desc->ptep,(long long)desc->pte, (long long)pte)) { //wrong	
	/************************************************/	
	
		/* Race detected. we should kill the process as it may have
		 * accessed inconsistent data on the newpage.
		 *
		 * XXX examine *desc->ptep to see what has
		 * actually happened?
		 */
		E("Race detected.");
		printk("hym: func %s, line %d, Race detected\n", __func__, __LINE__);
		return -1;
	}

	//printk("hym: func %s, line %d\n", __func__, __LINE__);
#endif

//	k2_measure("cas-end");	/* xzl: < 1us. seems reasonable */

	/*
	 * from migrate_page_copy()
	 * */
	if (PageError(page))
		SetPageError(newpage);
	if (PageReferenced(page))
		SetPageReferenced(newpage);
	if (PageUptodate(page))
		SetPageUptodate(newpage);
	if (TestClearPageActive(page)) {
		VM_BUG_ON(PageUnevictable(page));
		SetPageActive(newpage);
	} else if (TestClearPageUnevictable(page))
		SetPageUnevictable(newpage);
	if (PageChecked(page))
		SetPageChecked(newpage);
	if (PageMappedToDisk(page))
		SetPageMappedToDisk(newpage);
	//printk("hym: func %s, line %d\n", __func__, __LINE__);

	if (PageDirty(page)) {
		clear_page_dirty_for_io(page);
		/*
		 * Want to mark the page and the radix tree as dirty, and
		 * redo the accounting that clear_page_dirty_for_io undid,
		 * but we can't use set_page_dirty because that function
		 * is actually a signal that all of the page has become dirty.
		 * Whereas only part of our page may be dirty.
		 */
		if (PageSwapBacked(page))
			SetPageDirty(newpage);
		else
			__set_page_dirty_nobuffers(newpage);
	}

//	k2_measure("pageflags-end");

	//printk("hym: func %s, line %d\n", __func__, __LINE__);
	/* xzl: this migrates page's mlock flags? */
	mlock_migrate_page(newpage, page);
	ksm_migrate_page(newpage, page);
	/*
	 * Please do not reorder this without considering how mm/ksm.c's
	 * get_ksm_page() depends upon ksm_migrate_page() and PageSwapCache().
	 */
	ClearPageSwapCache(page);
	ClearPagePrivate(page);
	set_page_private(page, 0);
	
	//printk("hym: func %s, line %d\n", __func__, __LINE__);

	/*
	 * If any waiters have accumulated on the new page then
	 * wake them up.
	 * xzl: meaning someone wants to writeback page during migration? so
	 * let them proceed now?
	 * note: the new ptes are not up yet
	 */
	if (PageWriteback(newpage))
		end_page_writeback(newpage);

	page->mapping = NULL;
	unlock_page(newpage);
/*	// hym: cgroup is not used in linux 4.4, so I comment it
	mem_cgroup_end_migration(mem, page, newpage, true);
*/
	unlock_page(page);
	
	//printk("hym: func %s, line %d\n", __func__, __LINE__);

//	k2_measure("unlock-end");

	/*
	  	from unmap_and_move()
	 */
	
	// 4k page
	if(huge_page == 0){
		list_del(&page->lru);
		dec_zone_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
	}
	/* xzl: if all refs are successfully removed, the page will be freed.
	 * (right now pagecount is 1 ... until the page is flushed out of
	 * pagevec) */

	//printk("hym: func %s, line %d\n", __func__, __LINE__);

	//4k page
	if(huge_page == 0){  //huge_page is a global variable, bad idea....
		putback_lru_page(page);
	}else{ //2M page
		putback_active_hugepage(page);
	}
	
	D("oldpage back to lru. count %d ", page_count(page));
	//printk("oldpage back to lru. count %d \n", page_count(page)); //5.17
//	k2_measure("oldpage-lru");

	/*
	 * Move the new page to the LRU. If migration was not successful
	 * then this will free the page.
	 *
	 * xzl: note pages are first staged in a per-cpu per-lru pagevec then get
	 * moved to lru in batches. This has three implications:
	 * 1. PageLRU(page) may equal 0 after this call.
	 * 2. refcnt -1 for removing from isolated LRU;
	 * 	  refcnt + 1 for adding to pagevec by page_cache_get().
	 *    when removed from pagevec to LRU,  __pagevec_lru_add() will
	 * 	  call release_pages() to dec refcnt.
	 * 3. when __pagevec_lru_add() is called (every 14 pages --
	 *    PAGEVEC_SIZE is defined as 14), the cost of the putback_lru** is
	 * 	  much higher (10us vs normal 1us).
	 */
	
	//printk("hym: func %s, line %d\n", __func__, __LINE__);

	//4k page
	if(huge_page == 0){
		putback_lru_page(newpage);
	}else{ //2M page
		hugetlb_cgroup_migrate(page, newpage); //???
		putback_active_hugepage(newpage);
	}

	D("newpage %pK back in lru. PageLRU %d PageUnevictable %d "
			"Active %d count %d mapcount %d",
			newpage,
			PageLRU(newpage), PageUnevictable(newpage), PageActive(newpage),
			page_count(newpage), page_mapcount(newpage));
/*	
	printk("newpage %p back in lru. PageLRU %d PageUnevictable %d Active %d count %d mapcount %d\n", newpage, PageLRU(newpage), PageUnevictable(newpage), 
		PageActive(newpage), page_count(newpage), page_mapcount(newpage));
*/ //5.17
	/* xzl: XXX change the status in desc->mig and move the desc
	 * to the completion queue. */
//	k2_measure("newpage-lru");
	
	//printk("hym: func %s, line %d\n", __func__, __LINE__);

	return MIGRATEPAGE_SUCCESS;
}

static void dma_callback_dummy(void *param)
{
	E("called");
}

#if 0
/* in softirq -- deal with one desc. */
static void dma_callback_kthread_mig(void *param)
{
	struct mig_table *table = (struct mig_table *)param;
	mig_region *r = table->region;
	struct mmap_info *info = (struct mmap_info *) \
			((unsigned long)r + PAGE_SIZE * (NPAGES-1));
	int i, rc;

	k2_measure("softirq/nr");

	V("callback is invoked. tx %08x (npages %d in %d descs)",
			table->tx_id, table->num_pages, table->num_mig_descs);

	BUG_ON(!r);

	BUG_ON(!table->migdesc);

	for (i = 0; i < table->num_pages; i++) {
		rc = dma_complete_one_mig(table->pg_desc + i);
	}

	mig_enqueue(r, &(r->qcomp), table->migdesc);
	wake_up_interruptible(&info->wait_queue);

	/* mm refcnt has to be dec by the following */
	INIT_WORK(&(table->work), kernel_flush_work);

	schedule_work(&(table->work));
	/* atomic context, can't call mmput() here */
}
#endif

/* The generic softirq handler.
 * Deals with one desc that is either just copied or migrated.
 */

static int test_flag = 0;
static void dma_callback_kthread(void *param)
{
	struct mig_table *table = (struct mig_table *)param;
	mig_region *r = table->region;
	struct mmap_info *info = (struct mmap_info *) \
			((unsigned long)r + PAGE_SIZE * (NPAGES-1));
	int i, rc;
	int cpu;

	//printk("my_counter = %d\n", atomic_read(&my_counter));
	cmplet_time[test_flag++] = ktime_get();

#if MULTI_CHAN_SERIAL
	/*
	 *multi channels do DMA serially
	**/
	up(&sem);
#endif
	if(atomic_dec_and_test(&my_counter)){

		//printk("enter atomic_dec_and_test !!! \n");
		//printk("hym: func %s, line %d\n", __func__, __LINE__); //5.17
		//printk("&*&*&*&*&*&& enter dma_call_back_kthread&*&*&*&*&*\n"); //5.17


		k2_measure("softirq/nr");

		V("callback is invoked. tx %08x (npages %d in %d descs)",
				table->tx_id, table->num_pages, table->num_mig_descs);

		//printk("enter dma_callback_kthread!!!!!!!\n");

		BUG_ON(!r);

		BUG_ON(!table->migdesc);

		BUG_ON(!table->issue_one_desc_func);

		/* if we're doing migration, some cleanup work has to be done
		 * for each page. */
		if (table->issue_one_desc_func ==  issue_one_desc_mig) {
			//printk("&*&*&*&* dma_complete_one_mig() &*&*&*&\n"); //5.17
			//#if 0 //hym: test, remember to restore

			for (i = 0; i < table->num_pages; i++){
				rc = dma_complete_one_mig(table->pg_desc + i);

				/*			if(rc == MIGRATEPAGE_SUCCESS){
							printk("MIGRATEPAGE_SUCCESS really!!!???\n");
							}else{
							printk("MIGRATEPAGE_SUCCESS failed!!!!!\n");
							}
				 */

			}
			//#endif
		}

		mig_enqueue(r, &(r->qcomp), table->migdesc);
		k2_measure("cb:enqueue-end/nr");

		//printk("hym: func %s, line %d\n", __func__, __LINE__); //5.17

		wake_up_interruptible(&info->wait_queue);

		/* Schedule the geneirc kernel thread work.
		 *
		 * mm refcnt has to be dec by the following */
		INIT_WORK(&(table->work), kernel_flush_work);

		//	schedule_work(&(table->work));
		//	queue_work(system_unbound_wq, &(table->work));	// doesn't help much

		/* depending on which core the user caller is on, choose the corresponding
		 * core.
		 *
		 * consideration:
		 * - if we do nothing, ARM serves all irqs on core 0 by default.
		 * - distribute irqs to all cores help cache, but incurs mode switches
		 * 		on all cores
		 * - serve irq on local core help cache, but may hurt single thread case
		 *   	(see reason above) and the kernel thread compete with the user
		 *   	thread for cpu.
		 */ //printk("hym: func %s, line %d\n", __func__, __LINE__);//5.17

		cpu = (table->user_cpu + 1) % 4;
		V("schedule work on core %d", cpu);
		schedule_work_on(table->user_cpu, &(table->work));

		//printk("hym: func %s, line %d\n", __func__, __LINE__); //5.17

		/* atomic context, can't call mmput() here */
		
		printk("completion time: ");
		for(i = 0; i < NR_DEVICES; i++){
			printk("%ld  ", ktime_to_ns(cmplet_time[i]));
		}
		printk("\n");
		
		printk("data transfer time = %ld ns\n",ktime_to_ns(ktime_sub(cmplet_time[NR_DEVICES - 1], start_transfer[0])));

		printk("each disc time: ");
		for(i = 0; i < NR_DEVICES; i++){
			printk("%ld ", ktime_to_ns(ktime_sub(cmplet_time[i], start_transfer[i])));
		}
		printk("\n");
	}
}

/*
 * Obtain the lock on page, remove all ptes and migrate the page
 * to the newly allocated page in newpage.
 */
static int unmap_and_move_ks2(new_page_t get_new_page, unsigned long private,
			struct page *page, int force, enum migrate_mode mode,
			struct mig_pg_desc *desc)
{
	int rc = 0;
	int *result = NULL;
	struct page *newpage = get_new_page(page, private, &result);

	if (!newpage)
		return -ENOMEM;

	//xzl: why did page refcnt == 1 mean the page was freed?
	if (page_count(page) == 1) {
		/* page was freed from under us. So we are done. */
		goto out;
	}

	//xzl: if THP, we need to split the page before migrate?
	if (unlikely(PageTransHuge(page)))
		if (unlikely(split_huge_page(page)))
			goto out;

	rc = __unmap_and_move_ks2(page, newpage, force, mode, desc);

	if (unlikely(rc == MIGRATEPAGE_BALLOON_SUCCESS)) {
		/*
		 * A ballooned page has been migrated already.
		 * Now, it's the time to wrap-up counters,
		 * handle the page back to Buddy and return.
		 */
		dec_zone_page_state(page, NR_ISOLATED_ANON +
				    page_is_file_cache(page));
		balloon_page_free(page);
		return MIGRATEPAGE_SUCCESS;
	}
out:
	if (rc == MIGRATEPAGE_PENDING) {
		*result = rc;
		return rc;
	}

	/* xzl: the following will be done on the callback path */
	if (rc != -EAGAIN) {
		/*
		 * A page that has been migrated has all references
		 * removed and will be freed. A page that has not been
		 * migrated will have kepts its references and be
		 * restored.
		 *
		 * xzl: this moves the old page from isolated_anon / isolated_file
		 * list to where it belongs to.
		 *
		 * putback_lru_page() returns a previously isolated page to the proper
		 * lru
		 */
		list_del(&page->lru);
		dec_zone_page_state(page, NR_ISOLATED_ANON +
				page_is_file_cache(page));
		putback_lru_page(page);
	}

	/*
	 * Move the new page to the LRU. If migration was not successful
	 * then this will free the page.
	 */
	putback_lru_page(newpage);
	if (result) {
		if (rc)
			*result = rc;
		else
			*result = page_to_nid(newpage);
	}
	return rc;
}


/*
 * Counterpart of unmap_and_move_page() for hugepage migration.
 *
 * This function doesn't wait the completion of hugepage I/O
 * because there is no race between I/O and migration for hugepage.
 * Note that currently hugepage I/O occurs only in direct I/O
 * where no lock is held and PG_writeback is irrelevant,
 * and writeback status of all subpages are counted in the reference
 * count of the head page (i.e. if all subpages of a 2MB hugepage are
 * under direct I/O, the reference of the head page is 512 and a bit more.)
 * This means that when we try to migrate hugepage whose subpages are
 * doing direct I/O, some references remain after try_to_unmap() and
 * hugepage migration fails without data corruption.
 *
 * There is also no race when direct I/O is issued on the page under migration,
 * because then pte is replaced with migration swap entry and direct I/O code
 * will wait in the page fault for migration to complete.
 *
 * xzl:
 *
 * the race coming from migration and page IO seems to be the central issue.
 *
 * "hugepage" IO -- only direct IO?
 * THP IO seems very tricky issue so far?
 * direct IO on hugepage -- what does this mean? mmap'd?
 */
#if 0
static int unmap_and_move_huge_page(new_page_t get_new_page,
				unsigned long private, struct page *hpage,
				int force, enum migrate_mode mode)
{
	int rc = 0;
	int *result = NULL;
	struct page *new_hpage = get_new_page(hpage, private, &result);
	struct anon_vma *anon_vma = NULL;

	if (!new_hpage)
		return -ENOMEM;

	rc = -EAGAIN;

	if (!trylock_page(hpage)) {
		if (!force || mode != MIGRATE_SYNC)
			goto out;
		lock_page(hpage);
	}

	if (PageAnon(hpage))
		anon_vma = page_get_anon_vma(hpage);

	try_to_unmap(hpage, TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);

	if (!page_mapped(hpage))
		rc = move_to_new_page(new_hpage, hpage, 1, mode);

	if (rc)
		remove_migration_ptes(hpage, hpage);

	if (anon_vma)
		put_anon_vma(anon_vma);

	if (!rc)
		hugetlb_cgroup_migrate(hpage, new_hpage);

	unlock_page(hpage);
out:
	put_page(new_hpage);
	if (result) {
		if (rc)
			*result = rc;
		else
			*result = page_to_nid(new_hpage);
	}
	return rc;
}
#endif

/*
 * migrate_pages - migrate the pages specified in a list, to the free pages
 *		   supplied as the target for the page migration
 *
 * @from:		The list of pages to be migrated.
 * @get_new_page:	The function used to allocate free pages to be used
 *			as the target of the page migration.
 * @private:		Private data to be passed on to get_new_page()
 * @mode:		The migration mode that specifies the constraints for
 *			page migration, if any.
 * @reason:		The reason for page migration.
 *
 * The function returns after 10 attempts or if no pages are movable any more
 * because the list has become empty or no retryable pages exist any more.
 * The caller should call putback_lru_pages() to return pages to the LRU
 * or free list only if ret != 0.
 *
 * Returns the number of pages that were not migrated, or an error code.
 */
static int migrate_pages_ks2(struct list_head *from, new_page_t get_new_page,
		enum migrate_mode mode, int reason,
		struct mig_table *table)
{
	int retry = 1;
	int nr_failed = 0;
	int nr_succeeded = 0;
	int nr_pending = 0;
	int pass = 0;
	struct page *page;
	struct page *page2;
	struct page *opage, *npage;
	int swapwrite = current->flags & PF_SWAPWRITE;
	int rc;
	struct scatterlist *sg_src, *sg_dst;
	struct scatterlist *sgd, *sgs, *prev_sgd = NULL, *prev_sgs = NULL;
	struct dma_async_tx_descriptor *tx = NULL;
	dma_cookie_t cookie;
	unsigned long private = 0; /* arg to get_new_page */

	// xzl: this allows to write to swap.
	if (!swapwrite)
		current->flags |= PF_SWAPWRITE;

	I("npages %d", table->num_pages);

	sgs = sg_src = kmalloc(sizeof(struct scatterlist) * table->num_pages,
						GFP_KERNEL);
	sgd = sg_dst = kmalloc(sizeof(struct scatterlist) * table->num_pages,
						GFP_KERNEL);

	assert(sg_src && sg_dst);

	sg_init_table(sgd, table->num_pages);
	sg_init_table(sgs, table->num_pages);

	k2_measure("sglist-inited");	/* ~2us */

	for(pass = 0; pass < 10 && retry; pass++) {
		retry = 0;

		// xzl: go through each page
		// @page: loop cursor; @page2: temp storage
		// after every success, the page is deleted from page->lru (so we could
		// end up with a partially successful migration?)
		//
		// XXX we assume the list are all ''pending'' transfers
		list_for_each_entry_safe(page, page2, from, lru) {
			if (!in_softirq())
				cond_resched(); // xzl: yield cpu for each iteration...

			private = (unsigned long)(&(table->pg_desc[nr_pending]));
			rc = unmap_and_move_ks2(get_new_page, private,
						page, pass > 2, mode, table->pg_desc + nr_pending);
			k2_measure("unmapped-one");

			switch(rc) {
			case -ENOMEM:
				goto out;
			case -EAGAIN:
				retry++;
				break;
			case MIGRATEPAGE_SUCCESS:
				nr_succeeded++;
				break;
			case MIGRATEPAGE_PENDING:
				opage = table->pg_desc[nr_pending].page;
				npage = table->pg_desc[nr_pending].newpage;
				/* assemble two sg lists */
				BUG_ON((!sgs) || (!sgd));
				sg_set_page(sgs, opage, PAGE_SIZE, 0);
				sg_set_page(sgd, npage, PAGE_SIZE, 0);
#if 0			// used to verify that dma works (and does not mess up things)
				E("skipping pgcopy");
				sg_set_page(sgs, opage, 0, 0);
				sg_set_page(sgd, npage, 0, 0);
#endif
				/* Thanks to the IO coherency provided by MSMC, we don't have
				 * to map/unmap dma buffers using ARM's default dma ops
				 * such as arm_dma_map_sg().
				 *
				 * However, we must take care of the mapping between 40bit
				 * CPU phys addr and the 32bit edma addr that was assumed
				 * by MSMC.
				 * See phys40_to_dma32()
				 */
				BUG_ON((!sgs) || (!sgd));
				
				//modified by hym
				// dma address is the same as physical address on x86
				sgs->dma_address = page_to_phys(opage); //hym
				sgd->dma_address = page_to_phys(npage); //hym
				//sgs->dma_address = phys40_to_dma32(page_to_phys(opage));
				//sgd->dma_address = phys40_to_dma32(page_to_phys(npage));
				
				D("%d: phys_addr: src %llx (count %d) dst %llx (count %d) "
						"len %x", nr_pending,
						page_to_phys(opage), page_count(opage),
						page_to_phys(npage), page_count(npage),
						sg_dma_len(sgs));
				prev_sgs = sgs;
				prev_sgd = sgd;
				sgs = sg_next(sgs);
				sgd = sg_next(sgd);
				nr_pending ++;
				BUG_ON(nr_pending > MAX_NR_SG);
				break;
			default:
				/* Permanent failure */
				nr_failed++;
				break;
			}
		}
	}
	rc = nr_failed + retry;
out:
	if (nr_succeeded)
		count_vm_events(PGMIGRATE_SUCCESS, nr_succeeded);
	if (nr_failed) {
		count_vm_events(PGMIGRATE_FAIL, nr_failed);
		BUG();
	}
//	trace_mm_migrate_pages(nr_succeeded, nr_failed, mode, reason);	// xzl: create another one?

	BUG_ON(nr_succeeded && nr_pending);

	if (nr_pending) {
		BUG_ON(!chans[0]);

		/* finish the sg lists and submit */
		BUG_ON(!prev_sgs || !prev_sgd);
		sg_mark_end(prev_sgs);
		sg_mark_end(prev_sgd);

		table->pg_desc[nr_pending].page = NULL; /* mark end */

		k2_measure("sglist-ready");

		/*
		D("map_sg points to %pF", get_dma_ops(chan->device->dev)->map_sg);
		ndma = dma_map_sg(chan->device->dev, sg_src, nr_pending,
				DMA_BIDIRECTIONAL);
		BUG_ON(ndma != nr_pending);
		ndma = dma_map_sg(chan->device->dev, sg_dst, nr_pending,
						DMA_BIDIRECTIONAL);
		BUG_ON(ndma != nr_pending);
		*/

		/* Thanks to IO coherency, no map/unmap needed */
		tx = chans[0]->device->device_prep_dma_sg(chans[0],
				sg_dst, nr_pending, sg_src, nr_pending,
				DMA_CTRL_ACK | DMA_PREP_INTERRUPT
				  | DMA_COMPL_SKIP_DEST_UNMAP | DMA_COMPL_SKIP_SRC_UNMAP
		);
		BUG_ON(!tx);
//		k2_measure("dma-preped");

		tx->callback = dma_callback;
		tx->callback_param = (void *)table;

		cookie = dmaengine_submit(tx); // put to the chan's submitted list
		dma_async_issue_pending(chans[0]); // kick the chan if it is not busy

		k2_measure("dma-issued");
	}

	if (!swapwrite)
		current->flags &= ~PF_SWAPWRITE;

	kfree(sg_dst);
	kfree(sg_src);

	k2_measure("sglist-freed");
	return rc;
}

#if 0
// xzl: this is only used by soft_offline_huge_page() to tackle soft error.
// what if migrating huge pages in NUMA?
int migrate_huge_page(struct page *hpage, new_page_t get_new_page,
		      unsigned long private, enum migrate_mode mode)
{
	int pass, rc;

	for (pass = 0; pass < 10; pass++) {
		rc = unmap_and_move_huge_page(get_new_page, private,
						hpage, pass > 2, mode);
		switch (rc) {
		case -ENOMEM:
			goto out;
		case -EAGAIN:
			/* try again */
			cond_resched();
			break;
		case MIGRATEPAGE_SUCCESS:
			goto out;
		default:
			rc = -EIO;
			goto out;
		}
	}
out:
	return rc;
}
#endif

#ifdef CONFIG_NUMA

// xzl: get a new dest page for a given src page @p.
// this is passed as callback to  @migrate_pages
//
// given an array of @page_to_node (pointed by @private) and a src page @p,
// find the node that corresponding to @p
// allocate a dest page at the given @node?
#if 0  /* obsoleted */

/*
 * Move a list of individual pages
 */
struct page_to_node {
	unsigned long addr;
	struct page *page;
	int node;
	int status;
};

static struct page *new_page_node(struct page *p, unsigned long private,
		int **result)
{
	struct page_to_node *pm = (struct page_to_node *)private;
	struct page *pg;
	// xzl: this looks bad. linear search into pm array to look for page*
	while (pm->node != MAX_NUMNODES && pm->page != p)
		pm++;

	if (pm->node == MAX_NUMNODES)
		return NULL;

	*result = &pm->status;

	D("node %d max %d", pm->node, MAX_NUMNODES);

	//xzl: allocates one page only. For THP -- let automatic merge to happen?
	pg = alloc_pages_exact_node(pm->node,
				GFP_HIGHUSER_MOVABLE | GFP_THISNODE, 0);
	if (!pg) {
		E("failed to alloc pages from node %d -- stop", pm->node);
		BUG();
	}

	return pg;
}
#endif

/* don't rely searching into pm. instead, @private is the mig_pg_desc */
static struct page *new_page_node1(struct page *p, unsigned long private,
		int **result)
{
	struct mig_pg_desc *desc = (struct mig_pg_desc *)private;
	struct page *pg;
	int node = desc->mig->node; /* no lock needed */

	BUG_ON(!desc);
	BUG_ON(!(node == 0 || node == 1));

	*result = &desc->status;

	//xzl: allocates one page only. For THP -- let automatic merge to happen?
	pg = alloc_pages_exact_node(node,
				GFP_HIGHUSER_MOVABLE | GFP_THISNODE, 0);
	if (!pg) {
		E("failed to alloc pages from node %d -- stop", node);
		BUG();
	}

	return pg;
}

#if 0
/* directly grab the newpage as supplied by the user */
static struct page *new_page_noremap(struct page *p, unsigned long private,
		int **result)
{
	struct mig_pg_desc *desc = (struct mig_pg_desc *)private;
	struct page *page;
	struct vm_area_struct *vma;
	unsigned long paddr; /* user supplied new page vaddr */

	BUG_ON(!desc);

	paddr = base_to_vaddr(desc->mig->virt_base_dest);
	vma = find_vma(mm, paddr);

	if (!vma || paddr < vma->vm_start || !vma_migratable(vma)) {
		E("vma does not look right");
		BUG();
	}

	*result = &desc->status;

	page = follow_page(vma, paddr, FOLL_GET|FOLL_SPLIT);

	BUG_ON(IS_ERR(page));
	BUG_ON(!page);
	BUG_ON(PageReserved(page));
	BUG_ON(page_to_nid(page) == desc->node); /* XXX can we be nicer? */
	BUG_ON(page_mapcount(page) > 1);

	return page;
}
#endif

/*
 * Move a set of pages as indicated in the pm array. The addr
 * field must be set to the virtual address of the page to be moved
 * and the node number must contain a valid target node.
 * The pm array ends with node = MAX_NUMNODES.
 *
 * xzl: @migrate_all -- whether to move shared pages as well; set based on
 * MPOL_MF_MOVE_ALL flag passed by user through syscall.
 *
 * @table: we expect the caller to zalloc @table, and populate its
 * 			@region and @num_mig_descs.
 */
#if 0
static int do_move_page_to_node_array(struct mm_struct *mm,
				      int migrate_all, struct mig_table * table)
{
	int err;
	int i;
//	LIST_HEAD(pagelist);
	INIT_LIST_HEAD(&table->pagelist);

	assert(table);
	assert(table->num_pages);
	table->mm = mm;

	down_read(&mm->mmap_sem);

	/*
	 * Build a list of pages to migrate
	 */
//	for (pp = pm; pp->node != MAX_NUMNODES; pp++) {
	for (i = 0; i < table->num_pages; i++)
		struct vm_area_struct *vma;
		struct page *page;

		D("examining one page");

		err = -EFAULT;
		vma = find_vma(mm, pp->addr);
		if (!vma || pp->addr < vma->vm_start || !vma_migratable(vma)) {
			E("vma does not look right");
			goto set_status;
		}

		// xzl: this converts user-provided virt addr to page*
		// how expensive it this?
		page = follow_page(vma, pp->addr, FOLL_GET|FOLL_SPLIT);

		err = PTR_ERR(page);
		if (IS_ERR(page)) {
			E("page err");
			goto set_status;
		}

		err = -ENOENT;
		if (!page) {
			E("page non exist");
			goto set_status;
		}

		/* Use PageReserved to check for zero page */
		if (PageReserved(page)) {
			E("page reserved");
			goto put_and_set;
		}

		pp->page = page;
		err = page_to_nid(page);

		if (err == pp->node) {
			/*
			 * Node already in the right place
			 */
			E("page already on node");
			goto put_and_set;
		}

		err = -EACCES;
		if (page_mapcount(page) > 1 &&
				!migrate_all) {
			E("page mapcount > 1");
			goto put_and_set;
		}

		err = isolate_lru_page(page);
		if (!err) {
			D("okay: add one page");
			list_add_tail(&page->lru, &table->pagelist);
			inc_zone_page_state(page, NR_ISOLATED_ANON +
					    page_is_file_cache(page));
		} else
			// xzl -- EBUSY. this may happen when the page is still in
			// pagevec for batch processing
			E("iso page %pK from lru failed. err %d", page, err);

put_and_set:
		/*
		 * Either remove the duplicate refcount from
		 * isolate_lru_page() or drop the page ref if it was
		 * not isolated.
		 */
		put_page(page);
set_status:
		pp->status = err;
	}

	err = 0;
	if (!list_empty(&table->pagelist)) {
		// xzl: it seems that migrate_pages() might be called for reasons other
		// than syscall?
		err = migrate_pages_ks2(&table->pagelist, new_page_node1,
				MIGRATE_SYNC, MR_SYSCALL, table);
		if (err)
			putback_lru_pages(&table->pagelist);
	}

	up_read(&mm->mmap_sem);
	return err;
}
#endif

#if 0
/*
 * Migrate an array of page address onto an array of nodes and fill
 * the corresponding array of status.
 */
static int do_pages_move(struct mm_struct *mm, nodemask_t task_nodes,
			 unsigned long nr_pages,
			 const void __user * __user *pages,
			 const int __user *nodes,
			 int __user *status, int flags)
{
	struct page_to_node *pm;
	unsigned long chunk_nr_pages;
	unsigned long chunk_start;
	int err;

	err = -ENOMEM;
	pm = (struct page_to_node *)__get_free_page(GFP_KERNEL);
	if (!pm)
		goto out;

	migrate_prep();

	/*
	 * Store a chunk of page_to_node array in a page,
	 * but keep the last one as a marker
	 *
	 * xzl: this moves pages in batches.
	 */
	chunk_nr_pages = (PAGE_SIZE / sizeof(struct page_to_node)) - 1;

	for (chunk_start = 0;
	     chunk_start < nr_pages;
	     chunk_start += chunk_nr_pages) {
		int j;

		if (chunk_start + chunk_nr_pages > nr_pages)
			chunk_nr_pages = nr_pages - chunk_start;

		/* fill the chunk pm with addrs and nodes from user-space */
		for (j = 0; j < chunk_nr_pages; j++) {
			const void __user *p;
			int node;

			err = -EFAULT;
			if (get_user(p, pages + j + chunk_start))
				goto out_pm;
			pm[j].addr = (unsigned long) p;

			if (get_user(node, nodes + j + chunk_start))
				goto out_pm;

			err = -ENODEV;
			if (node < 0 || node >= MAX_NUMNODES)
				goto out_pm;

			if (!node_state(node, N_MEMORY))
				goto out_pm;

			err = -EACCES;
			if (!node_isset(node, task_nodes))
				goto out_pm;

			pm[j].node = node;
		}

		/* End marker for this chunk */
		pm[chunk_nr_pages].node = MAX_NUMNODES;

		/* Migrate this chunk */
		err = do_move_page_to_node_array(mm, pm,
						 flags & MPOL_MF_MOVE_ALL);
		if (err < 0)
			goto out_pm;

		/* Return status information */
		for (j = 0; j < chunk_nr_pages; j++)
			if (put_user(pm[j].status, status + j + chunk_start)) {
				err = -EFAULT;
				goto out_pm;
			}
	}
	err = 0;

out_pm:
	free_page((unsigned long)pm);
out:
	return err;
}
#endif

/*
 * Determine the nodes of an array of pages and store it in an array of status.
 */
static void do_pages_stat_array(struct mm_struct *mm, unsigned long nr_pages,
				const void __user **pages, int *status)
{
	unsigned long i;

	down_read(&mm->mmap_sem);

	for (i = 0; i < nr_pages; i++) {
		unsigned long addr = (unsigned long)(*pages);
		struct vm_area_struct *vma;
		struct page *page;
		int err = -EFAULT;

		vma = find_vma(mm, addr);
		if (!vma || addr < vma->vm_start)
			goto set_status;

		page = follow_page(vma, addr, 0);

		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto set_status;

		err = -ENOENT;
		/* Use PageReserved to check for zero page */
		if (!page || PageReserved(page))
			goto set_status;

		err = page_to_nid(page);
		W("vaddr %08lx page* %08x node %d", addr, (int32_t)page, err);
set_status:
		*status = err;

		pages++;
		status++;
	}

	up_read(&mm->mmap_sem);
}

/*
 * Determine the nodes of a user array of pages and store it in
 * a user array of status.
 */
static int do_pages_stat(struct mm_struct *mm, unsigned long nr_pages,
			 const void __user * __user *pages,
			 int __user *status)
{
#define DO_PAGES_STAT_CHUNK_NR 16
	const void __user *chunk_pages[DO_PAGES_STAT_CHUNK_NR];
	int chunk_status[DO_PAGES_STAT_CHUNK_NR];

	while (nr_pages) {
		unsigned long chunk_nr;

		chunk_nr = nr_pages;
		if (chunk_nr > DO_PAGES_STAT_CHUNK_NR)
			chunk_nr = DO_PAGES_STAT_CHUNK_NR;

		if (copy_from_user(chunk_pages, pages, chunk_nr * sizeof(*chunk_pages)))
			break;

		do_pages_stat_array(mm, chunk_nr, chunk_pages, chunk_status);

		if (copy_to_user(status, chunk_status, chunk_nr * sizeof(*status)))
			break;

		pages += chunk_nr;
		status += chunk_nr;
		nr_pages -= chunk_nr;
	}
	return nr_pages ? -EFAULT : 0;
}

#if 0 	// xzl --> migrate.c
/*
 * Move a list of pages in the address space of the currently executing
 * process.
 * xzl: a thin wrapper does permission check, etc
 */
SYSCALL_DEFINE6(move_pages, pid_t, pid, unsigned long, nr_pages,
		const void __user * __user *, pages,
		const int __user *, nodes,
		int __user *, status, int, flags)
{
	const struct cred *cred = current_cred(), *tcred;
	struct task_struct *task;
	struct mm_struct *mm;
	int err;
	nodemask_t task_nodes;

	/* Check flags */
	if (flags & ~(MPOL_MF_MOVE|MPOL_MF_MOVE_ALL))
		return -EINVAL;

	if ((flags & MPOL_MF_MOVE_ALL) && !capable(CAP_SYS_NICE))
		return -EPERM;

	/* Find the mm_struct */
	rcu_read_lock();
	task = pid ? find_task_by_vpid(pid) : current;
	if (!task) {
		rcu_read_unlock();
		return -ESRCH;
	}
	get_task_struct(task);

	/*
	 * Check if this process has the right to modify the specified
	 * process. The right exists if the process has administrative
	 * capabilities, superuser privileges or the same
	 * userid as the target process.
	 */
	tcred = __task_cred(task);
	if (!uid_eq(cred->euid, tcred->suid) && !uid_eq(cred->euid, tcred->uid) &&
	    !uid_eq(cred->uid,  tcred->suid) && !uid_eq(cred->uid,  tcred->uid) &&
	    !capable(CAP_SYS_NICE)) {
		rcu_read_unlock();
		err = -EPERM;
		goto out;
	}
	rcu_read_unlock();

 	err = security_task_movememory(task);
 	if (err)
		goto out;

	task_nodes = cpuset_mems_allowed(task); // xzl: the task's cpuset
	mm = get_task_mm(task);
	put_task_struct(task);

	if (!mm)
		return -EINVAL;

	// xzl: per move_pages()'s semantics, when passed in @nodes as NULL,
	// don't move any pages but return each page's resident node
	if (nodes)
		err = do_pages_move(mm, task_nodes, nr_pages, pages,
				    nodes, status, flags);
	else
		err = do_pages_stat(mm, nr_pages, pages, status);

	mmput(mm);
	return err;

out:
	put_task_struct(task);
	return err;
}
#endif

#if 0
/*
 * Call migration functions in the vma_ops that may prepare
 * memory in a vm for migration. migration functions may perform
 * the migration for vmas that do not have an underlying page struct.
 */
int migrate_vmas(struct mm_struct *mm, const nodemask_t *to,
	const nodemask_t *from, unsigned long flags)
{
 	struct vm_area_struct *vma;
 	int err = 0;

	for (vma = mm->mmap; vma && !err; vma = vma->vm_next) {
 		if (vma->vm_ops && vma->vm_ops->migrate) {
 			err = vma->vm_ops->migrate(vma, to, from, flags);
 			if (err)
 				break;
 		}
 	}
 	return err;
}
#endif

#ifdef CONFIG_NUMA_BALANCING
/*
 * Returns true if this is a safe migration target node for misplaced NUMA
 * pages. Currently it only checks the watermarks which crude
 */

static bool migrate_balanced_pgdat(struct pglist_data *pgdat,
                                   unsigned long nr_migrate_pages)
{
        int z;
        for (z = pgdat->nr_zones - 1; z >= 0; z--) {
                struct zone *zone = pgdat->node_zones + z;

                if (!populated_zone(zone))
                        continue;

                if (!zone_reclaimable(zone))
                        continue;

                /* Avoid waking kswapd by allocating pages_to_migrate pages. */
                if (!zone_watermark_ok(zone, 0,
                                       high_wmark_pages(zone) +
                                       nr_migrate_pages,
                                       0, 0))
                        continue;
                return true;
        }
        return false;
}

#if 0
static bool migrate_balanced_pgdat(struct pglist_data *pgdat,
				   unsigned long nr_migrate_pages)
{
	int z;
	for (z = pgdat->nr_zones - 1; z >= 0; z--) {
		struct zone *zone = pgdat->node_zones + z;

		if (!populated_zone(zone))
			continue;

		if (zone->all_unreclaimable)
			continue;

		/* Avoid waking kswapd by allocating pages_to_migrate pages. */
		if (!zone_watermark_ok(zone, 0,
				       high_wmark_pages(zone) +
				       nr_migrate_pages,
				       0, 0))
			continue;
		return true;
	}
	return false;
}
#endif
//copy from x86 migrate.c hym
static struct page *alloc_misplaced_dst_page(struct page *page,
                                           unsigned long data,
                                           int **result)
{
        int nid = (int) data;
        struct page *newpage;

        newpage = __alloc_pages_node(nid,
                                         (GFP_HIGHUSER_MOVABLE |
                                          __GFP_THISNODE | __GFP_NOMEMALLOC |
                                          __GFP_NORETRY | __GFP_NOWARN) &
                                         ~(__GFP_IO | __GFP_FS), 0);

        return newpage;
}

#if 0
static struct page *alloc_misplaced_dst_page(struct page *page,
					   unsigned long data,
					   int **result)
{
	int nid = (int) data;
	struct page *newpage;

	newpage = alloc_pages_exact_node(nid,
					 (GFP_HIGHUSER_MOVABLE | GFP_THISNODE |
					  __GFP_NOMEMALLOC | __GFP_NORETRY |
					  __GFP_NOWARN) &
					 ~GFP_IOFS, 0);
	if (newpage)
		page_nid_xchg_last(newpage, page_nid_last(page));

	return newpage;
}
#endif

/*
 * page migration rate limiting control.
 * Do not migrate more than @pages_to_migrate in a @migrate_interval_millisecs
 * window of time. Default here says do not migrate more than 1280M per second.
 * If a node is rate-limited then PTE NUMA updates are also rate-limited. However
 * as it is faults that reset the window, pte updates will happen unconditionally
 * if there has not been a fault since @pteupdate_interval_millisecs after the
 * throttle window closed.
 */
static unsigned int migrate_interval_millisecs __read_mostly = 100;
static unsigned int pteupdate_interval_millisecs __read_mostly = 1000;
static unsigned int ratelimit_pages __read_mostly = 128 << (20 - PAGE_SHIFT);

/* Returns true if NUMA migration is currently rate limited */
bool migrate_ratelimited(int node)
{
	pg_data_t *pgdat = NODE_DATA(node);

	if (time_after(jiffies, pgdat->numabalancing_migrate_next_window +
				msecs_to_jiffies(pteupdate_interval_millisecs)))
		return false;

	if (pgdat->numabalancing_migrate_nr_pages < ratelimit_pages)
		return false;

	return true;
}

/* Returns true if the node is migrate rate-limited after the update */
bool numamigrate_update_ratelimit(pg_data_t *pgdat, unsigned long nr_pages)
{
	bool rate_limited = false;

	/*
	 * Rate-limit the amount of data that is being migrated to a node.
	 * Optimal placement is no good if the memory bus is saturated and
	 * all the time is being spent migrating!
	 */
	spin_lock(&pgdat->numabalancing_migrate_lock);
	if (time_after(jiffies, pgdat->numabalancing_migrate_next_window)) {
		pgdat->numabalancing_migrate_nr_pages = 0;
		pgdat->numabalancing_migrate_next_window = jiffies +
			msecs_to_jiffies(migrate_interval_millisecs);
	}
	if (pgdat->numabalancing_migrate_nr_pages > ratelimit_pages)
		rate_limited = true;
	else
		pgdat->numabalancing_migrate_nr_pages += nr_pages;
	spin_unlock(&pgdat->numabalancing_migrate_lock);

	return rate_limited;
}

int numamigrate_isolate_page(pg_data_t *pgdat, struct page *page)
{
	int page_lru;

	VM_BUG_ON(compound_order(page) && !PageTransHuge(page));

	/* Avoid migrating to a node that is nearly full */
	if (!migrate_balanced_pgdat(pgdat, 1UL << compound_order(page)))
		return 0;

	if (isolate_lru_page(page))
		return 0;

	/*
	 * migrate_misplaced_transhuge_page() skips page migration's usual
	 * check on page_count(), so we must do it here, now that the page
	 * has been isolated: a GUP pin, or any other pin, prevents migration.
	 * The expected page count is 3: 1 for page's mapcount and 1 for the
	 * caller's pin and 1 for the reference taken by isolate_lru_page().
	 */
	if (PageTransHuge(page) && page_count(page) != 3) {
		putback_lru_page(page);
		return 0;
	}

	page_lru = page_is_file_cache(page);
	mod_zone_page_state(page_zone(page), NR_ISOLATED_ANON + page_lru,
				hpage_nr_pages(page));

	/*
	 * Isolating the page has taken another reference, so the
	 * caller's reference can be safely dropped without the page
	 * disappearing underneath us during migration.
	 */
	put_page(page);
	return 1;
}

/*
 * Attempt to migrate a misplaced page to the specified destination
 * node. Caller is expected to have an elevated reference count on
 * the page that will be dropped by this function before returning.
 */
// copy from migrate.c hym
int migrate_misplaced_page(struct page *page, struct vm_area_struct *vma,
                           int node)
{
        pg_data_t *pgdat = NODE_DATA(node);
        int isolated;
        int nr_remaining;
        LIST_HEAD(migratepages);

        /*
         * Don't migrate file pages that are mapped in multiple processes
         * with execute permissions as they are probably shared libraries.
         */
        if (page_mapcount(page) != 1 && page_is_file_cache(page) &&
            (vma->vm_flags & VM_EXEC))
                goto out;

        /*
         * Rate-limit the amount of data that is being migrated to a node.
         * Optimal placement is no good if the memory bus is saturated and
         * all the time is being spent migrating!
         */
        if (numamigrate_update_ratelimit(pgdat, 1))
                goto out;

        isolated = numamigrate_isolate_page(pgdat, page);
        if (!isolated)
                goto out;

        list_add(&page->lru, &migratepages);
        nr_remaining = migrate_pages(&migratepages, alloc_misplaced_dst_page,
                                     NULL, node, MIGRATE_ASYNC,
                                     MR_NUMA_MISPLACED);
			//Q: why not migrate_pages-ks2????
        if (nr_remaining) {
                if (!list_empty(&migratepages)) {
                        list_del(&page->lru);
                        dec_zone_page_state(page, NR_ISOLATED_ANON +
                                        page_is_file_cache(page));
                        putback_lru_page(page);
                }
                isolated = 0;
        } else
                count_vm_numa_event(NUMA_PAGE_MIGRATE);
        BUG_ON(!list_empty(&migratepages));
        return isolated;

out:
        put_page(page);
        return 0;
}

#if 0
int migrate_misplaced_page(struct page *page, int node)
{
	pg_data_t *pgdat = NODE_DATA(node);
	int isolated;
	int nr_remaining;
	LIST_HEAD(migratepages);

	/*
	 * Don't migrate pages that are mapped in multiple processes.
	 * TODO: Handle false sharing detection instead of this hammer
	 */
	if (page_mapcount(page) != 1)
		goto out;

	/*
	 * Rate-limit the amount of data that is being migrated to a node.
	 * Optimal placement is no good if the memory bus is saturated and
	 * all the time is being spent migrating!
	 */
	if (numamigrate_update_ratelimit(pgdat, 1))
		goto out;

	isolated = numamigrate_isolate_page(pgdat, page);
	if (!isolated)
		goto out;

	list_add(&page->lru, &migratepages);
	nr_remaining = migrate_pages(&migratepages, alloc_misplaced_dst_page,
				     node, MIGRATE_ASYNC, MR_NUMA_MISPLACED);
	if (nr_remaining) {
		putback_lru_pages(&migratepages);
		isolated = 0;
	} else
		count_vm_numa_event(NUMA_PAGE_MIGRATE);
	BUG_ON(!list_empty(&migratepages));
	return isolated;

out:
	put_page(page);
	return 0;
}
#endif
#endif /* CONFIG_NUMA_BALANCING */

#if defined(CONFIG_NUMA_BALANCING) && defined(CONFIG_TRANSPARENT_HUGEPAGE)
/*
 * Migrates a THP to a given target node. page must be locked and is unlocked
 * before returning.
 */
//copy from migrate.c hym
extern void inc_zone_state(struct zone *, enum zone_stat_item);
extern void dec_zone_state(struct zone *, enum zone_stat_item);
int migrate_misplaced_transhuge_page(struct mm_struct *mm,
                                struct vm_area_struct *vma,
                                pmd_t *pmd, pmd_t entry,
                                unsigned long address,
                                struct page *page, int node)
{
        spinlock_t *ptl;
        pg_data_t *pgdat = NODE_DATA(node);
        int isolated = 0;
        struct page *new_page = NULL;
        int page_lru = page_is_file_cache(page);
        unsigned long mmun_start = address & HPAGE_PMD_MASK;
        unsigned long mmun_end = mmun_start + HPAGE_PMD_SIZE;
        pmd_t orig_entry;

        /*
         * Rate-limit the amount of data that is being migrated to a node.
         * Optimal placement is no good if the memory bus is saturated and
         * all the time is being spent migrating!
         */
        if (numamigrate_update_ratelimit(pgdat, HPAGE_PMD_NR))
                goto out_dropref;

        new_page = alloc_pages_node(node,
                (GFP_TRANSHUGE | __GFP_THISNODE) & ~__GFP_RECLAIM,
                HPAGE_PMD_ORDER);
        if (!new_page)
                goto out_fail;

        isolated = numamigrate_isolate_page(pgdat, page);
        if (!isolated) {
                put_page(new_page);
                goto out_fail;
        }

        if (mm_tlb_flush_pending(mm))
                flush_tlb_range(vma, mmun_start, mmun_end);

        /* Prepare a page as a migration target */
        __set_page_locked(new_page);
        SetPageSwapBacked(new_page);

        /* anon mapping, we can simply copy page->mapping to the new page: */
        new_page->mapping = page->mapping;
        new_page->index = page->index;
        migrate_page_copy(new_page, page);
        WARN_ON(PageLRU(new_page));

        /* Recheck the target PMD */
        mmu_notifier_invalidate_range_start(mm, mmun_start, mmun_end);
        ptl = pmd_lock(mm, pmd);
        if (unlikely(!pmd_same(*pmd, entry) || page_count(page) != 2)) {
fail_putback:
                spin_unlock(ptl);
                mmu_notifier_invalidate_range_end(mm, mmun_start, mmun_end);

                /* Reverse changes made by migrate_page_copy() */
                if (TestClearPageActive(new_page))
                        SetPageActive(page);
                if (TestClearPageUnevictable(new_page))
                        SetPageUnevictable(page);

                unlock_page(new_page);
                put_page(new_page);             /* Free it */

                /* Retake the callers reference and putback on LRU */
                get_page(page);
                putback_lru_page(page);
                mod_zone_page_state(page_zone(page),
                         NR_ISOLATED_ANON + page_lru, -HPAGE_PMD_NR);

                goto out_unlock;
        }

        orig_entry = *pmd;
        entry = mk_pmd(new_page, vma->vm_page_prot);
        entry = pmd_mkhuge(entry);
        entry = maybe_pmd_mkwrite(pmd_mkdirty(entry), vma);

        /*
         * Clear the old entry under pagetable lock and establish the new PTE.
         * Any parallel GUP will either observe the old page blocking on the
         * page lock, block on the page table lock or observe the new page.
         * The SetPageUptodate on the new page and page_add_new_anon_rmap
         * guarantee the copy is visible before the pagetable update.
         */
        flush_cache_range(vma, mmun_start, mmun_end);
        page_add_anon_rmap(new_page, vma, mmun_start);
        pmdp_huge_clear_flush_notify(vma, mmun_start, pmd);
        set_pmd_at(mm, mmun_start, pmd, entry);
        flush_tlb_range(vma, mmun_start, mmun_end);
        update_mmu_cache_pmd(vma, address, &entry);

        if (page_count(page) != 2) {
                set_pmd_at(mm, mmun_start, pmd, orig_entry);
                flush_tlb_range(vma, mmun_start, mmun_end);
                mmu_notifier_invalidate_range(mm, mmun_start, mmun_end);
                update_mmu_cache_pmd(vma, address, &entry);
                page_remove_rmap(new_page);
                goto fail_putback;
        }

        mlock_migrate_page(new_page, page);
        set_page_memcg(new_page, page_memcg(page));
        set_page_memcg(page, NULL);
        page_remove_rmap(page);

        spin_unlock(ptl);
        mmu_notifier_invalidate_range_end(mm, mmun_start, mmun_end);

        /* Take an "isolate" reference and put new page on the LRU. */
        get_page(new_page);
        putback_lru_page(new_page);

        unlock_page(new_page);
        unlock_page(page);
        put_page(page);                 /* Drop the rmap reference */
        put_page(page);                 /* Drop the LRU isolation reference */

        count_vm_events(PGMIGRATE_SUCCESS, HPAGE_PMD_NR);
        count_vm_numa_events(NUMA_PAGE_MIGRATE, HPAGE_PMD_NR);

        mod_zone_page_state(page_zone(page),
                        NR_ISOLATED_ANON + page_lru,
                        -HPAGE_PMD_NR);
        return isolated;

out_fail:
        count_vm_events(PGMIGRATE_FAIL, HPAGE_PMD_NR);
out_dropref:
        ptl = pmd_lock(mm, pmd);
        if (pmd_same(*pmd, entry)) {
                entry = pmd_modify(entry, vma->vm_page_prot);
                set_pmd_at(mm, mmun_start, pmd, entry);
                update_mmu_cache_pmd(vma, address, &entry);
        }
        spin_unlock(ptl);

out_unlock:
        unlock_page(page);
        put_page(page);
        return 0;
}


#if 0
int migrate_misplaced_transhuge_page(struct mm_struct *mm,
				struct vm_area_struct *vma,
				pmd_t *pmd, pmd_t entry,
				unsigned long address,
				struct page *page, int node)
{
	unsigned long haddr = address & HPAGE_PMD_MASK;
	pg_data_t *pgdat = NODE_DATA(node);
	int isolated = 0;
	struct page *new_page = NULL;
	struct mem_cgroup *memcg = NULL;
	int page_lru = page_is_file_cache(page);

	/*
	 * Don't migrate pages that are mapped in multiple processes.
	 * TODO: Handle false sharing detection instead of this hammer
	 */
	if (page_mapcount(page) != 1)
		goto out_dropref;

	/*
	 * Rate-limit the amount of data that is being migrated to a node.
	 * Optimal placement is no good if the memory bus is saturated and
	 * all the time is being spent migrating!
	 */
	if (numamigrate_update_ratelimit(pgdat, HPAGE_PMD_NR))
		goto out_dropref;

	new_page = alloc_pages_node(node,
		(GFP_TRANSHUGE | GFP_THISNODE) & ~__GFP_WAIT, HPAGE_PMD_ORDER);
	if (!new_page)
		goto out_fail;

	page_nid_xchg_last(new_page, page_nid_last(page));

	isolated = numamigrate_isolate_page(pgdat, page);
	if (!isolated) {
		put_page(new_page);
		goto out_fail;
	}

	/* Prepare a page as a migration target */
	__set_page_locked(new_page);
	SetPageSwapBacked(new_page);

	/* anon mapping, we can simply copy page->mapping to the new page: */
	new_page->mapping = page->mapping;
	new_page->index = page->index;
	migrate_page_copy(new_page, page);
	WARN_ON(PageLRU(new_page));

	/* Recheck the target PMD */
	spin_lock(&mm->page_table_lock);
	if (unlikely(!pmd_same(*pmd, entry))) {
		spin_unlock(&mm->page_table_lock);

		/* Reverse changes made by migrate_page_copy() */
		if (TestClearPageActive(new_page))
			SetPageActive(page);
		if (TestClearPageUnevictable(new_page))
			SetPageUnevictable(page);
		mlock_migrate_page(page, new_page);

		unlock_page(new_page);
		put_page(new_page);		/* Free it */

		unlock_page(page);
		putback_lru_page(page);

		count_vm_events(PGMIGRATE_FAIL, HPAGE_PMD_NR);
		isolated = 0;
		goto out;
	}

	/*
	 * Traditional migration needs to prepare the memcg charge
	 * transaction early to prevent the old page from being
	 * uncharged when installing migration entries.  Here we can
	 * save the potential rollback and start the charge transfer
	 * only when migration is already known to end successfully.
	 */
	mem_cgroup_prepare_migration(page, new_page, &memcg);

	entry = mk_pmd(new_page, vma->vm_page_prot);
	entry = pmd_mknonnuma(entry);
	entry = maybe_pmd_mkwrite(pmd_mkdirty(entry), vma);
	entry = pmd_mkhuge(entry);

	page_add_new_anon_rmap(new_page, vma, haddr);

	set_pmd_at(mm, haddr, pmd, entry);
	update_mmu_cache_pmd(vma, address, &entry);
	page_remove_rmap(page);
	/*
	 * Finish the charge transaction under the page table lock to
	 * prevent split_huge_page() from dividing up the charge
	 * before it's fully transferred to the new page.
	 */
	mem_cgroup_end_migration(memcg, page, new_page, true);
	spin_unlock(&mm->page_table_lock);

	unlock_page(new_page);
	unlock_page(page);
	put_page(page);			/* Drop the rmap reference */
	put_page(page);			/* Drop the LRU isolation reference */

	count_vm_events(PGMIGRATE_SUCCESS, HPAGE_PMD_NR);
	count_vm_numa_events(NUMA_PAGE_MIGRATE, HPAGE_PMD_NR);

out:
	mod_zone_page_state(page_zone(page),
			NR_ISOLATED_ANON + page_lru,
			-HPAGE_PMD_NR);
	return isolated;

out_fail:
	count_vm_events(PGMIGRATE_FAIL, HPAGE_PMD_NR);
out_dropref:
	unlock_page(page);
	put_page(page);
	return 0;
}
#endif
#endif /* CONFIG_NUMA_BALANCING */

#endif /* CONFIG_NUMA */

/*
 * Can be called by process or kernel tasklet.
 *
 * Assemble the page list on the go
 * @user: 1 means user context.
 * @parent: parent tx id. -1 if from user.
 * return: the # of descs flushed
 *
 * Much of the idea comes from do_move_page_to_node_array()
 * */
static atomic_t usr_nr_tx = ATOMIC_INIT(0);
static atomic_t ker_nr_tx = ATOMIC_INIT(0xf0000000);

static int flush_single_qreq(mig_region *r, int user, struct mm_struct *mm,
		int parent)
{
	struct mig_table *table = NULL;
	mig_desc 	*desc;
	int err, i, npages, j;
	unsigned long paddr;
	color_t old_clr, new_clr, c;
	/* info@ is right after the mig_region. see if_open() */
	struct mmap_info *info = (struct mmap_info *) \
			((unsigned long)r + PAGE_SIZE * (NPAGES-1));

	k2_measure("flush-start");

	assert(r);
	assert(mm);

	if (user) {
		/* xzl: drain pagevec to LRU so that they can be
		 * isolated. expensive? */
//		migrate_prep();
		old_clr = COLOR_USR;
		new_clr = COLOR_KER;
	} else {
		old_clr = COLOR_KER;
		new_clr = COLOR_USR;
	}

	/* Flush all descs from req queue to issued queue, and build an isolated
	 * LRU list on the go.
	 *
	 * Change the queue color as needed.
	 */
	i = 0; /* # of descs */
	npages = 0;

	down_read(&mm->mmap_sem);
	while (1) {
		struct vm_area_struct *vma;
		struct page *page;

		desc = mig_dequeue(r, &(r->qreq));

		/*
		 * Done flushing the queue
		 * */
		if ((!desc)) {
			if (user || (!user && (i == 0))) {
				/* User done flushing the queue or kernel has flushed nothing
				 * from the queue. change the queue color.
				 */
				c = migqueue_set_color_if_empty(r, &(r->qreq),
						new_clr);
				if (c == -1) /* queue becomes non empty due to race. try again */
					continue;
				else { /* color changed okay */
					if (c != old_clr) {
						E("bug: color %d expected %d. i %d idx %d",
								c, old_clr, i, r->qreq.head.index);
						BUG();
					}
					if (user)
						W("user tx sets color: %s -> %s", color_str(old_clr),
							color_str(new_clr));
					else {
						W("tx %08x sets color: %s -> %s", parent, color_str(old_clr),
							color_str(new_clr));
						if (!migqueue_is_empty(&r->qcomp))
									wake_up_interruptible(&info->wait_queue);
						k2_measure("clr:->usr");
						k2_flush_measure_format();
					}
					break;
				}
			} else if (!user) {
				/* Kernel has done flushing something from the queue.
				 * Color remains COLOR_KER.
				 * Proceed to the xfer. */
				break;
			} else
				BUG();
		}

		/*
		 * We have a new desc to work on.
		 */

		/* do we have a fresh mig_table yet? */
		if (!table) {
			/* create & init a new mig table. */
//			table = kmalloc(sizeof(struct mig_table), GFP_KERNEL);
			BUG_ON(sizeof(struct mig_table) >= \
					(1<<MIGTABLE_PAGE_ORDER) * PAGE_SIZE);
			table = (struct mig_table *)__get_free_pages(GFP_KERNEL,
					MIGTABLE_PAGE_ORDER);
			assert(table);
			D("mig_table sz %d max_nr_sg %d", sizeof(struct mig_table),
					MAX_NR_SG);
			INIT_LIST_HEAD(&table->pagelist);
		}

		/* set each page in the desc */
		for (j = 0, paddr = (unsigned long)(desc->virt_base << MIG_BLOCK_ALIGN_OFFSET);
				j < (1 << desc->order);
				j++, paddr += PAGE_SIZE) {
			vma = find_vma(mm, paddr);
			if (!vma || paddr < vma->vm_start || !vma_migratable(vma)) {
				E("vma does not look right");
				BUG();
			}

			if (desc->node < 0 || desc->node > 1) {
				E("desc.word0 %08x node %d", desc->word0, desc->node);
				BUG();
			}

			// xzl: this converts user-provided virt addr to page*
			// how expensive it this?
			page = follow_page(vma, paddr, FOLL_GET|FOLL_SPLIT);

			/* See do_move_page_to_node_array() for comments */
			BUG_ON(IS_ERR(page));
			BUG_ON(!page);
			BUG_ON(PageReserved(page));
			BUG_ON(page_to_nid(page) == desc->node); /* XXX can we be nicer? */
			BUG_ON(page_mapcount(page) > 1);

			err = isolate_lru_page(page);
			if (err) {
				// xzl -- EBUSY. this may happen when the page is still in
				// pagevec for batch processing
				E("iso page %pK phys %llx from lru failed. err %d",
						page, page_to_phys(page), err);

				BUG();
			}

			D("okay: add one page");
			list_add_tail(&page->lru, &table->pagelist);
			inc_zone_page_state(page, NR_ISOLATED_ANON +
					    page_is_file_cache(page));

			table->pg_desc[npages].mig = desc;
			/* more pg_desc fields, e.g. page, newpage, will be filled by
			 * __unmap_and_move_ks2()
			 */

			/*
			 * Either remove the duplicate refcount from
			 * isolate_lru_page() or drop the page ref if it was
			 * not isolated.
			 */
			put_page(page);

			table->pg_desc[npages].status = 0;  /* more status code? */

			if (npages ++ == MAX_NR_SG) {
				/* over dma sglist size XXX */
				E("bug: npages %d too big for a sglist", npages);
				BUG();
			}
		}

		mig_enqueue(r, &(r->qissued), desc);
		i ++;
	}

	k2_measure("pglist-ready");

	if (i == 0) {
		/* no transfers to do. no matter whether we come from user or
		 * kernel thread, the previously got mm is not longer needed.
		 * so put it. otherwise, the mm will be leaked (held forever).
		 */
		BUG_ON(table);
		mmput(mm);
		I("non desc read. mmput()");
		goto out;
	}

	/* We have some descs. */
	BUG_ON(list_empty(&table->pagelist));
	I("total %d descs (%d pages) read from req queue", i, npages);

	table->region = r;
	table->mm = mm;
	table->num_mig_descs = i;
	table->num_pages = npages;
	if (user) {
		/* debug */
		table->tx_id = atomic_inc_return(&usr_nr_tx);
		W("user starts tx %08x", table->tx_id);
	} else {
		table->tx_id = atomic_inc_return(&ker_nr_tx);
		W("tx %08x starts tx %08x", parent, table->tx_id);
	}

	err = migrate_pages_ks2(&table->pagelist, new_page_node1,
			MIGRATE_SYNC, MR_SYSCALL, table);
	BUG_ON(err);

	k2_measure("mig-end");

	/*
	if (err)
		putback_lru_pages(&table->pagelist);
	*/

	/* XXX: we should examine status. for pages who fail to move (e.g. EBUSY)
	 * move the desc to qerr
	 */
out:
	up_read(&mm->mmap_sem);
	k2_measure("flush-end");
	W("==== done ==== ");
	return i;
}

/* The caller will get_mm() so it won't disappear.
 *
 * No virt mm involved. We expect that the mig desc provides virt src addr and
 * virt dest addr.
 *
 * We assemble two sglists on the go.
 */
#if 0
static int flush_single_qreq_noremap(mig_region *r, int user,
		struct mm_struct *mm, int parent)
{
	struct mig_table *table = NULL;
	/* info@ is right after the mig_region. see if_open() */
	struct mmap_info *info = (struct mmap_info *) \
			((unsigned long)r + PAGE_SIZE * (NPAGES-1));

	struct scatterlist *sg_src = NULL, *sg_dst = NULL;
	struct scatterlist *sgd = NULL, *sgs = NULL, *prev_sgd = NULL, *prev_sgs = NULL;
	struct dma_async_tx_descriptor *tx = NULL;
	dma_cookie_t cookie;

	mig_desc 	*desc;
	int i, npages, j;
	unsigned long paddr, paddr1;
	color_t old_clr, new_clr, c;

	k2_measure("flush-start/nr");

	assert(r);
	assert(mm);

	if (user) {
		/* xzl: drain pagevec to LRU so that they can be
		 * isolated. expensive? */
//		migrate_prep();
		old_clr = COLOR_USR;
		new_clr = COLOR_KER;
	} else {
		old_clr = COLOR_KER;
		new_clr = COLOR_USR;
	}

	/* Flush all descs from req queue to issued queue, and build an isolated
	 * LRU list on the go.
	 *
	 * Change the queue color as needed.
	 */
	i = 0; /* # of descs */
	npages = 0;

	down_read(&mm->mmap_sem); /* neede for follow_page()? */
	while (1) {
		struct vm_area_struct *vma, *vma1;
		struct page *opage, *npage;

//		if (i >= 2)
//			break;
//		if (!user)
//			msleep(1);

		desc = mig_dequeue(r, &(r->qreq));

		/*
		 * Done flushing the queue
		 * */
		if ((!desc)) {
			if (user || (!user && (i == 0))) {
				/* User done flushing the queue or kernel has flushed nothing
				 * from the queue. change the queue color.
				 */
				c = migqueue_set_color_if_empty(r, &(r->qreq),
						new_clr);
				if (c == -1) /* queue becomes non empty due to race. try again */
					continue;
				else { /* color changed okay */
					if (c != old_clr) {
						E("bug: color %d expected %d. i %d idx %d",
								c, old_clr, i, r->qreq.head.index);
						BUG();
					}
					if (user)
						I("user tx sets color: %s -> %s", color_str(old_clr),
							color_str(new_clr));
					else {
						I("tx %08x sets color: %s -> %s", parent, color_str(old_clr),
							color_str(new_clr));
						/* notify the user -- perhaps unnecessary */
						k2_measure("prior-wakeup/nr");
						if (!migqueue_is_empty(&r->qcomp))
							wake_up_interruptible(&info->wait_queue);
						k2_measure("clr:->usr/nr");
						k2_flush_measure_format();
					}
					break;
				}
			} else if (!user) {
				/* Kernel has done flushing something from the queue.
				 * Color remains COLOR_KER.
				 * Proceed to the xfer. */
				break;
			} else
				BUG();
		}

		/*
		 * We have a new desc to work on.
		 */
//		if (!user)
//			cond_resched();

		/* do we have a fresh mig_table yet? */
		if (!table) {
			/* create & init a new mig table.
			 * we still need it to pass mm, mig_region between user and
			 * irq. */
			BUG_ON(sgs || sgd);
			if (sizeof(struct mig_table) >=
					(1<<MIGTABLE_PAGE_ORDER) * PAGE_SIZE) {
				E("sizeof(struct mig_table) %d bytes > %lu",
						sizeof(struct mig_table), PAGE_SIZE);
				BUG();
			}

			table = (struct mig_table *)__get_free_pages(GFP_KERNEL,
					MIGTABLE_PAGE_ORDER);
			assert(table);
			V("mig_table sz %d max_nr_sg %d", sizeof(struct mig_table),
					MAX_NR_SG);
//			INIT_LIST_HEAD(&table->pagelist);

			/* assemble two sg lists -- we don't know how many pages we'll
			 * have yet. */
			sgs = sg_src = kmalloc(sizeof(struct scatterlist) * MAX_NR_SG,
								GFP_KERNEL);
			sgd = sg_dst = kmalloc(sizeof(struct scatterlist) * MAX_NR_SG,
								GFP_KERNEL);

			assert(sg_src && sg_dst);

			sg_init_table(sgd, MAX_NR_SG);
			sg_init_table(sgs, MAX_NR_SG);
		}

		/* set each page in the desc */
		for (j = 0, paddr = base_to_virt(desc->virt_base),
				paddr1 = base_to_virt(desc->virt_base_dest);
				j < (1 << desc->order);
				j++, paddr += PAGE_SIZE, paddr1 += PAGE_SIZE) {

			vma = find_vma(mm, paddr);
			vma1 = find_vma(mm, paddr1);
			if (!vma || paddr < vma->vm_start || !vma_migratable(vma)) {
				E("vma for vaddr %lx does not look right", paddr);
				BUG();
			}

			if (!vma1 || paddr1 < vma1->vm_start || !vma_migratable(vma1)) {
				E("vma1 for vaddr %lx does not look right", paddr1);
				BUG();
			}

			D("paddr %08lx vma %pK paddr1 %08lx vma1 %pK",
					paddr, vma, paddr1, vma1);

			// xzl: this converts user-provided virt addr to page*
			// how expensive it this?
			// -- do we need to pass in FOLL_GET?
			opage = follow_page(vma, paddr, FOLL_SPLIT);
			npage = follow_page(vma1, paddr1, FOLL_SPLIT);

			/* See do_move_page_to_node_array() for comments */
			BUG_ON(IS_ERR(opage));
			if (!opage) {
				D("paddr %08lx vma %pK paddr1 %08lx vma1 %pK",
									paddr, vma, paddr1, vma1);
				BUG_ON(!opage);
			}
			BUG_ON(page_mapcount(opage) > 1);

			BUG_ON(IS_ERR(npage));
			/* this is possible, as the dest virt address range may not
			 * be populated with phys pages yet. */
			BUG_ON(!npage);
			BUG_ON(page_mapcount(npage) > 1);

			if (npages ++ == MAX_NR_SG) {
				/* over dma sglist size XXX */
				E("bug: npages %d too big for a sglist", npages);
				BUG();
			}

			/* append to the sg lists */
			BUG_ON((!sgs) || (!sgd));

			sg_set_page(sgs, opage, PAGE_SIZE, 0);
			sg_set_page(sgd, npage, PAGE_SIZE, 0);

			//modified by hym
			//sgs->dma_address = phys40_to_dma32(page_to_phys(opage));
			//sgd->dma_address = phys40_to_dma32(page_to_phys(npage));
			// dma address = phys address on x86
			sgs->dma_address = page_to_phys(opage);
			sgd->dma_address = page_to_phys(npage);
			

			D("%d: phys_addr: src %llx (count %d) dst %llx (count %d) "
					"len %x", nr_pending,
					page_to_phys(opage), page_count(opage),
					page_to_phys(npage), page_count(npage),
					sg_dma_len(sgs));

			prev_sgs = sgs;
			prev_sgd = sgd;

			sgs = sg_next(sgs);
			sgd = sg_next(sgd);
		}

		mig_enqueue(r, &(r->qissued), desc);
		i ++;
	}

	k2_measure("pglist-ready/nr");

	if (i == 0) {
		/* no transfers to do. no matter whether we come from user or
		 * kernel thread, the previously got mm is not longer needed.
		 * so put it. otherwise, the mm will be leaked (held forever).
		 */
		BUG_ON(table);
		V("non desc read. mm %pK mmput()", mm);
		mmput(mm);
		goto out;
	}

	/* We have some descs. */
//	BUG_ON(list_empty(&table->pagelist));
	D("mm %pK total %d descs (%d pages) read from req queue", mm, i, npages);

	table->region = r;
	table->mm = mm;
	table->num_mig_descs = i;
	table->num_pages = npages;
	/* debug */
	if (user) {
		table->tx_id = atomic_inc_return(&usr_nr_tx);
		W("user starts tx %08x", table->tx_id);
	} else {
		table->tx_id = atomic_inc_return(&ker_nr_tx);
		W("tx %08x starts tx %08x", parent, table->tx_id);
	}

	BUG_ON(!chan);

	/* finish the sg lists and submit */
	BUG_ON(!prev_sgs || !prev_sgd);
	sg_mark_end(prev_sgs);
	sg_mark_end(prev_sgd);

	table->pg_desc[npages].page = NULL; /* mark end */

	k2_measure("sglist-ready/nr");

	/*
	D("map_sg points to %pF", get_dma_ops(chan->device->dev)->map_sg);
	ndma = dma_map_sg(chan->device->dev, sg_src, nr_pending,
			DMA_BIDIRECTIONAL);
	BUG_ON(ndma != nr_pending);
	ndma = dma_map_sg(chan->device->dev, sg_dst, nr_pending,
					DMA_BIDIRECTIONAL);
	BUG_ON(ndma != nr_pending);
	*/

	/* Thanks to IO coherency, no map/unmap needed */
	tx = chan->device->device_prep_dma_sg(chan,
			sg_dst, npages, sg_src, npages,
			DMA_CTRL_ACK | DMA_PREP_INTERRUPT
			  | DMA_COMPL_SKIP_DEST_UNMAP | DMA_COMPL_SKIP_SRC_UNMAP
	);

	BUG_ON(!tx);
	k2_measure("dma-preped/nr");

	tx->callback = dma_callback_noremap;
	tx->callback_param = (void *)table;

	cookie = dmaengine_submit(tx); // put to the chan's submitted list
	dma_async_issue_pending(chan); // kick the chan if it is not busy

	k2_measure("dma-issued/nr");

out:
	up_read(&mm->mmap_sem);
	k2_measure("flush-end/nr");

	kfree(sg_dst);
	kfree(sg_src);

	V("==== done ==== ");
	return i;
}
#endif

/******************** added by hym **********************/
// this function should be in mm/memory.c, but I copy it here
#if 0
int follow_page_range(struct vm_area_struct *vma,
                unsigned long address, int count, struct page **ppage,
                unsigned int flags)
{
        pgd_t *pgd;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *ptep, pte;
        spinlock_t *ptl;
        struct mm_struct *mm = vma->vm_mm;
        int ret = 0;
        int n = 0;      /* how many pages we've collected so far */

//      printk("xzl:%s:address %08lx count %d", __func__, address, count);

        /* xzl: we don't need to grab @page_table_lock? */
new_pgd:
        pgd = pgd_offset(mm, address);
        if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
                BUG(); /* XXX: next pgd? */

        pud = pud_offset(pgd, address);
        if (pud_none(*pud))
                BUG();          /* xzl: XXX next pud? */
        if (unlikely(pud_bad(*pud)))
                BUG();

new_pmd:
        pmd = pmd_offset(pud, address);
        if (pmd_none(*pmd)) {
                E("bug:*pmd=%llx (vma %pK pgd %pK pmd %pK addr %08lx\n",
                                *pmd, vma, pgd, pmd, address);
                BUG();  /* XXX next pmd? */
        } else /* debugging */
                V("*pmd=%llx seems okay (vma %pK pgd %pK pmd %pK addr %08lx\n",
                        *pmd, vma, pgd, pmd, address);

//split_fallthrough:
        if (unlikely(pmd_bad(*pmd)))
                BUG();

        /* xzl - now reached a new pte table (pointed by @pmd) */
        ptep = pte_offset_map_lock(mm, pmd, address, &ptl);

        while (1) {
                pte = *ptep;

                if (!pte_present(pte)) {
                        E("!pte_present: address %08lx pte %llx", address, pte);
                        E("(vma %pK pgd %pK pmd %pK addr %08lx",
                                                        vma, pgd, pmd, address);
                        ppage[n] = NULL;
                        BUG();
                }

                if ((flags & FOLL_WRITE) && !pte_write(pte)) {
                        ppage[n] = NULL;
                        BUG();
                }

                ppage[n] = vm_normal_page(vma, address, pte);

                if (unlikely(!ppage[n])) { /* xzl -- page does not exist */
                        BUG();
                }

                if (flags & FOLL_GET)
                        get_page_foll(ppage[n]);

                if (flags & FOLL_TOUCH) {
                        if ((flags & FOLL_WRITE) &&
                            !pte_dirty(pte) && !PageDirty(ppage[n]))
                                set_page_dirty(ppage[n]);
                        /*
                         * pte_mkyoung() would be more correct here, but atomic care
                         * is needed to avoid losing the dirty bit: it is easier to use
                         * mark_page_accessed().
                         */
                        mark_page_accessed(ppage[n]);
                }

                /* page looks good */
                ret ++;

                if (++n == count) {
                        pte_unmap_unlock(ptep, ptl);
                        break; /* all set */
                }

                address += PAGE_SIZE;

                /* we have entered a new pgd/pmd...
                 * (the ptep is still valid) */
                if (!(address & (PGDIR_SIZE - 1))) {
                        pte_unmap_unlock(ptep, ptl);
                        goto new_pgd;
                }

                if (!(address & (PMD_SIZE - 1))) {
                        pte_unmap_unlock(ptep, ptl);
                        goto new_pmd;
                }
                ptep ++;
        }

        return ret;
}
#endif
/********************** end hym **********************************/




/* caller has to free the returned @table
 * XXX we use the global @chan.
 * */
static struct mig_table *issue_one_desc_copy(mig_region *r,
		mig_desc *desc, int do_irq, struct mm_struct *mm)
{
	struct page **opage, **npage;
	int count;
	struct mig_table *table;
	struct scatterlist *sg_src = NULL, *sg_dst = NULL;
	struct scatterlist *sgd = NULL, *sgs = NULL, *prev_sgd = NULL, *prev_sgs = NULL;
	struct dma_async_tx_descriptor *tx = NULL;
	dma_cookie_t cookie;
	unsigned long tx_flags = DMA_CTRL_ACK |
			  DMA_COMPL_SKIP_DEST_UNMAP | DMA_COMPL_SKIP_SRC_UNMAP;
	unsigned long paddr, paddr1;
	struct vm_area_struct *vma, *vma1;
	int i, j;
	struct mmap_info *info = region_to_mmap_info(r);

	//printk("func: %s, line: %d\n",__func__, __LINE__); //hym 5.17
	int each_chan_page_count;
	struct scatterlist *sg_src_n[NR_DEVICES], *sg_dst_n[NR_DEVICES];
	struct scatterlist *m_sgs, *m_sgd;
	struct dma_async_tx_descriptor *m_tx[NR_DEVICES];
	dma_cookie_t m_cookie[NR_DEVICES];

	ktime_t start, end;
	
	start = ktime_get();

#ifdef EMU_HUGETLB
	/* note the way we determine source node is a bit dirty... */
	unsigned long dummy_dma_dst = the_dummy_dma[desc->node];
	unsigned long dummy_dma_src = the_dummy_dma[1 - desc->node];

	assert(desc->node >=0 && desc->node <= 1);
	assert(dummy_dma_dst && dummy_dma_dst);
#endif

	if (sizeof(struct mig_table) >= (1<<MIGTABLE_PAGE_ORDER) * PAGE_SIZE) {
		E("sizeof(struct mig_table) %d bytes > %lu",
				sizeof(struct mig_table), (1<<MIGTABLE_PAGE_ORDER) * PAGE_SIZE);
		BUG();
	}

	table = (struct mig_table *)__get_free_pages(GFP_KERNEL,
			MIGTABLE_PAGE_ORDER);
	assert(table);
	V("mig_table sz %d max_nr_sg %d", sizeof(struct mig_table),
			MAX_NR_SG);

	/* assemble two sg lists */
	count = (1 << desc->order);
	//printk("count = %d\n", count); //5.17

	/*************************/
	//count = 1;   //hym: just copy one page
	/************************/


	if(count > MAX_NR_SG){
		printk("count = %d, MAX_NR_SG =%d, count > MAX_NR_SG ************\n", count, MAX_NR_SG);
	}
	BUG_ON(count > MAX_NR_SG);

	sgs = sg_src = kmalloc(sizeof(struct scatterlist) * count,
						GFP_KERNEL);
	sgd = sg_dst = kmalloc(sizeof(struct scatterlist) * count,
						GFP_KERNEL);

	assert(sg_src && sg_dst);

	sg_init_table(sgd, count);
	sg_init_table(sgs, count);

	k2_measure("sglist-inited");

	/*
	 * set each page in the desc
	 * */
	
	//printk("func: %s, line: %d\n",__func__, __LINE__); //hym 5.17

	paddr = base_to_virt(desc->virt_base);
	paddr1 = base_to_virt(desc->virt_base_dest);
/*
	printk("kernel: desc->virt_base = %08lx, desc->virt_base_dest = %08lx\n",
		desc->virt_base, desc->virt_base_dest); //hym
	printk("kernel: desc->order = %d\n", desc->order); //hym
 */

	down_read(&mm->mmap_sem); /* neede for follow_page()? */

	vma = find_vma(mm, paddr);
	vma1 = find_vma(mm, paddr1);

	V("paddr %08lx vma %pK paddr1 %08lx vma1 %pK",
			paddr, vma, paddr1, vma1);

/*	printk("virtual page addr:paddr %08lx vma %pK paddr1 %08lx vma1 %pK",
			 paddr, vma, paddr1, vma1); //hym
*/
	/*
	 * if the following checks fail, consider:
	 * 1. whether the vma has been touched and faulted in by user
	 * 2. whether the user has already freed the vma or even quited the program.
	 */
	if (!vma || paddr < vma->vm_start || !vma_migratable(vma)) {
		E("vma %pK for vaddr %lx does not look right", vma, paddr);
		if (vma)
			E("vma %pK ?? (%08lx -- %08lx)", vma, vma->vm_start, vma->vm_end);
		BUG();
	}

	V("vma %pK ok (%08lx -- %08lx)", vma, vma->vm_start, vma->vm_end);

	if (!vma1 || paddr1 < vma1->vm_start || !vma_migratable(vma1)) {
		E("vma1 for vaddr %lx does not look right", paddr1);
		if (vma1)
			E("vma1 %pK (%08lx -- %08lx)", vma1, vma1->vm_start, vma1->vm_end);
		BUG();
	}

	opage = kmalloc(sizeof(struct page *) * count, GFP_KERNEL);
	npage = kmalloc(sizeof(struct page *) * count, GFP_KERNEL);
	BUG_ON(!opage || !npage);

	// -- do we need to pass in FOLL_GET?
	
	//printk("func: %s, line: %d\n",__func__, __LINE__); //hym 5.17

/*	// compare original follow page and my follow_huge_page
	struct page *page_test;
	page_test = follow_page(vma, paddr, FOLL_GET | FOLL_SPLIT | FOLL_DUMP);	
	printk("\nsrc physicall address is: %llx\n",page_to_phys(page_test));

	page_test = follow_page(vma, paddr1, FOLL_GET | FOLL_SPLIT | FOLL_DUMP);
	printk("dst physicall address is: %llx\n\n",page_to_phys(page_test));
*/	// end compare

	if(desc->huge_page == 0){ //4k page
		follow_page_range(vma, paddr, count, opage, FOLL_SPLIT);
		follow_page_range(vma1, paddr1, count, npage, FOLL_SPLIT);
	}else{ // 2M page
		follow_huge_page_range(vma, paddr, count, opage, FOLL_SPLIT);
		follow_huge_page_range(vma1, paddr1, count, npage, FOLL_SPLIT);
	}

	//printk("func: %s, line: %d\n",__func__, __LINE__); //hym //5.17
	up_read(&mm->mmap_sem);

	for (j = 0; j < count; j++) {
		/* See do_move_page_to_node_array() for comments */
		BUG_ON(IS_ERR(opage[j]));
		if (!opage[j])
			BUG();
		BUG_ON(page_mapcount(opage[j]) > 1);

		BUG_ON(IS_ERR(npage[j]));
		/* this is possible, as the dest virt address range may not
		 * be populated with phys pages yet. */
		//printk("page_mapcount(npage[j]) = %d\n", page_mapcount(npage[j]));//
		BUG_ON(!npage[j]);
		BUG_ON(page_mapcount(npage[j]) > 1); // -1 unmapped, 0 single mapping, 1 shared

		/* append to the sg lists */
		BUG_ON((!sgs) || (!sgd));

#ifdef EMU_HUGETLB
		/* do dummy transfers. the dma engine driver won't
		 * care about page* anyway. */
		sg_set_page(sgs, 0, /* page* */ EMU_HUGETLB_PG_SIZE, 0);
		sg_set_page(sgd, 0, /* page* */EMU_HUGETLB_PG_SIZE, 0);

		sgs->dma_address = dummy_dma_src;
		sgd->dma_address = dummy_dma_dst;
		printk("func: %s, line: %d\n",__func__, __LINE__); //hym
#else

		// hym: diff 4k page and 2M page
		if(desc->huge_page == 0){
			// 4k page
			sg_set_page(sgs, opage[j], PAGE_SIZE, 0);
			sg_set_page(sgd, npage[j], PAGE_SIZE, 0);
		}else{  //2M page
			sg_set_page(sgs, opage[j], HUGE_PAGE_SIZE, 0);
			sg_set_page(sgd, npage[j], HUGE_PAGE_SIZE, 0);
		}

		//modified by hym 
		//sgs->dma_address = phys40_to_dma32(page_to_phys(opage[j]));
		//sgd->dma_address = phys40_to_dma32(page_to_phys(npage[j]));
		// dma address = phys address on x86
		sgs->dma_address = page_to_phys(opage[j]);
		sgd->dma_address = page_to_phys(npage[j]);

		/*verify that dma is correct, the problem may be that phys_addr is wrong*/
/*		sgs->dma_address = page_to_phys(virt_to_page(__get_free_page(GFP_KERNEL|__GFP_DMA)));
		sgd->dma_address = page_to_phys(virt_to_page(__get_free_page(GFP_KERNEL|__GFP_DMA)));
		u64 *src_test, *dst_test;
		dma_addr_t src_dma_test, dst_dma_test;
		src_test = dma_alloc_coherent(info->chan->device->dev, 4096, &src_dma_test, GFP_KERNEL);
		dst_test = dma_alloc_coherent(info->chan->device->dev, 4096, &dst_dma_test, GFP_KERNEL);

		sgs->dma_address = src_dma_test;
		sgd->dma_address = dst_dma_test;
*/		/* end of verity*/

		//hym: just set a random value, should change to the data size later...
		if(desc->huge_page == 0){ //4k page
			sgs->dma_length = 4096;
		}else{ //2M page
			//sgs->dma_length = HUGE_PAGE_SIZE;
			sgs->dma_length = 2 * 1024 * 1024;
		}
#endif
		V("%d: phys_addr: src %llx (count %d) dst %llx (count %d) "
				"len %x", j,
				page_to_phys(opage[j]), page_count(opage[j]),
				page_to_phys(npage[j]), page_count(npage[j]),
				sg_dma_len(sgs));

/*		printk("%d: phys_addr: src %llx (count %d) dst %llx (count %d) "
				"len %x", j,
				sgs->dma_address, page_count(opage[j]),
				sgd->dma_address, page_count(npage[j]),
				sgs->dma_length);
*/
		prev_sgs = sgs;
		prev_sgd = sgd;

		sgs = sg_next(sgs);
		sgd = sg_next(sgd);
	}
	//printk("func: %s, line: %d\n",__func__, __LINE__); //hym //5.17
	kfree(opage);
	kfree(npage);

	/* table setup */
	table->region = r;
	table->mm = mm;
	table->num_mig_descs = 1;
	table->num_pages = count;
	table->migdesc = desc;
	table->issue_one_desc_func = issue_one_desc_copy;
	/* XXX only necessary in proc context */
	table->user_cpu = smp_processor_id();

	BUG_ON(!prev_sgs || !prev_sgd);
	sg_mark_end(prev_sgs);
	sg_mark_end(prev_sgd);
	table->pg_desc[count].page = NULL; /* mark end */

	k2_measure("sglist-ready");

	if (do_irq)
		tx_flags |= DMA_PREP_INTERRUPT;

	/*
	 * Now we can use all NR_DEVICES channels.
	 *
	 * Add/modify code here if want to specify which channels we want to use
	 * especially, we want to use channels in diff controller: e.g. 0,8 channles
	 *
	**/
	each_chan_page_count = count / NR_DEVICES; // using NR_DEVICES channels
	m_sgs = sg_src;
	m_sgd = sg_dst;
	
	for(i = 0; i < NR_DEVICES; i++){     
		//printk("**************** i = %d\n", i);
		sg_src_n[i] = m_sgs;
		sg_dst_n[i] = m_sgd;

		for(j = 0; j < each_chan_page_count; j++){
			m_sgs = sg_next(m_sgs);
			m_sgd = sg_next(m_sgd);
			//printk("   **************** j = %d\n", j);
		}
	}

	//my_conter is used to sync up in dma_callback_kthread
	atomic_set(&my_counter, NR_DEVICES);

	test_flag = 0;

#if MULTI_CHAN_SERIAL	
	sema_init(&sem, 1);
#endif

	for(i = 0; i < NR_DEVICES; i++){

#if MULTI_CHAN_SERIAL
		down(&sem);
#endif
		m_tx[i] = chans[i]->device->device_prep_dma_sg(chans[i], sg_dst_n[i],
				each_chan_page_count, sg_src_n[i], each_chan_page_count, tx_flags);
		BUG_ON(!m_tx[i]);
		//BUG_ON(!tx);

		if (do_irq) {
			m_tx[i]->callback = dma_callback_kthread;
			//tx->callback = dma_callback_kthread;
			m_tx[i]->callback_param = (void *)table;
			//tx->callback_param = (void *)table;
		}

		//comment these temporally, remember to restore !!
		m_cookie[i] = dmaengine_submit(m_tx[i]);
		//cookie = dmaengine_submit(tx); // put to the chan's submitted list
		dma_async_issue_pending(chans[i]);
		//dma_async_issue_pending(info->chan); // kick the chan if it is not busy
		start_transfer[i] = ktime_get();
	}
	
	end = ktime_get();
	printk("config total time = %ld ns\n", ktime_to_ns(ktime_sub(end, start)));
	printk("start transfert time: ");
	for(i = 0; i < NR_DEVICES; i++){
		printk("%ld  ", ktime_to_ns(start_transfer[i]));
	}
	printk("\n");

	k2_measure("dma-issued");

	kfree(sg_dst);
	kfree(sg_src);
	
	//printk("func: %s, line: %d\n",__func__, __LINE__); //hym //5.17
	return table;
}

static struct mig_table *issue_one_desc_mig(mig_region *r,
		mig_desc *desc, int do_irq, struct mm_struct *mm)
{
	struct page **opage;
	int count;
	struct mig_table *table;
	struct scatterlist *sg_src = NULL, *sg_dst = NULL;
	struct scatterlist *sgd = NULL, *sgs = NULL, *prev_sgd = NULL, *prev_sgs = NULL;
	//struct scatterlist *sg_src_n[NR_DEVICES], *sg_dst_n[NR_DEVICES]; 
	struct dma_async_tx_descriptor *tx = NULL;
	dma_cookie_t cookie;
	unsigned long tx_flags = DMA_CTRL_ACK |
			  DMA_COMPL_SKIP_DEST_UNMAP | DMA_COMPL_SKIP_SRC_UNMAP;
	
	//modifyed by hym
	//unsigned long paddr;
	unsigned long paddr;

	ktime_t start, end;

	struct vm_area_struct *vma;
	int i, j;
	struct mmap_info *info = region_to_mmap_info(r);

	int each_chan_page_count;
	struct scatterlist *sg_src_n[NR_DEVICES], *sg_dst_n[NR_DEVICES];
	struct scatterlist *m_sgs, *m_sgd;
	struct dma_async_tx_descriptor *m_tx[NR_DEVICES];
	dma_cookie_t m_cookie[NR_DEVICES];

	start = ktime_get();

#ifdef EMU_HUGETLB
	/* note the way we determine source node is a bit dirty... */
	unsigned long dummy_dma_dst = the_dummy_dma[desc->node];
	unsigned long dummy_dma_src = the_dummy_dma[1 - desc->node];

	assert(desc->node >=0 && desc->node <= 1);
	assert(dummy_dma_dst && dummy_dma_dst);
#endif

	if(desc->huge_page == 1){
		huge_page = 1; //global variable, bad idea...
	}else{
		huge_page = 0;
	}


	/* start a fresh table */
	if (sizeof(struct mig_table) >= (1<<MIGTABLE_PAGE_ORDER) * PAGE_SIZE) {
		E("sizeof(struct mig_table) %d bytes > %lu",
				sizeof(struct mig_table), (1<<MIGTABLE_PAGE_ORDER) * PAGE_SIZE);
		BUG();
	}
	table = (struct mig_table *)__get_free_pages(GFP_KERNEL,
			MIGTABLE_PAGE_ORDER);
	assert(table);
	V("mig_table %pK sz %d max_nr_sg %d", table, sizeof(struct mig_table),
			MAX_NR_SG);
	INIT_LIST_HEAD(&table->pagelist);

	/* initialize two sg lists */
	count = (1 << desc->order);

	//printk("count = %d\n",count); //5.17

	if(count > MAX_NR_SG){
		printk("count = %d, MAX_NR_SG =%d, count > MAX_NR_SG ************\n", count, MAX_NR_SG);
	}

	BUG_ON(count > MAX_NR_SG);

	sgs = sg_src = kmalloc(sizeof(struct scatterlist) * count,
						GFP_KERNEL);
	sgd = sg_dst = kmalloc(sizeof(struct scatterlist) * count,
						GFP_KERNEL);
	assert(sg_src && sg_dst);

	sg_init_table(sgs, count);
	sg_init_table(sgd, count);

	k2_measure("sglist-inited");
	D("sgs %pK sgd %pK mm %pK", sgs, sgd, mm);

	/*
	 * set each page in the desc
	 * */

	paddr = base_to_virt(desc->virt_base);
/*	printk("kernel: desc->virt_base = %08lx, desc->virt_base_dest = %08lx\n",
		desc->virt_base, desc->virt_base_dest); //hym
	printk("kernel: desc->order = %d\n", desc->order); //hym
*/ //5.17
	down_read(&mm->mmap_sem); /* needed for follow_page()? */

	//printk("migrate-x86.c: %s line: %d\n", __func__, __LINE__); //5.17

	vma = find_vma(mm, paddr);
	if (!vma || paddr < vma->vm_start || !vma_migratable(vma)) {
		E("vma for vaddr %lx does not look right", paddr);
		BUG();
	}
	V("paddr %08lx vma %pK", paddr, vma);
/*
	printk("paddr %08lx vma %pK", paddr, vma);
	printk("migrate-x86.c: %s line: %d\n", __func__, __LINE__);
*/ //5.17
#if 0
	{	/* -- test -- */
		struct page *page;
		I("begin follow page");
		page = follow_page(vma, paddr, FOLL_GET|FOLL_SPLIT);
		I("end follow page");
		return;
	}
#endif

	/* look up old page* */
	opage = kmalloc(sizeof(struct page *) * count, GFP_KERNEL);
	BUG_ON(!opage);

	/* we could furture combine pte replacement with the following; however,
	 * that removes the need for anon_map and makes our code
	 * fundamentally depends on page->mapcount == 1. bad.
	 *
	 * XXX do we need to pass in FOLL_GET?
	 */
	//should be restore, I just comment it temporarily
	if(desc->huge_page == 0){// hym: 4k page
		follow_page_range(vma, paddr, count, opage, FOLL_SPLIT | FOLL_GET);
	}else{ // hym: 2M page
		follow_huge_page_range(vma, paddr, count, opage, FOLL_SPLIT | FOLL_GET);
	}
	//printk("migrate-x86.c: %s line: %d\n", __func__, __LINE__); //5.17

	k2_measure("followed-pages");
	
	//printk("count = %d\n",count); //5.17

	/* the new pages will be allocated in the following loop */
	for (j = 0; j < count; j++) {
		int err;
		struct page *page = opage[j];
		struct page *newpage;
		struct anon_vma *anon_vma = NULL;

		/* See do_move_page_to_node_array() for comments */
		BUG_ON(IS_ERR(page));
		if (!page)
			BUG();
		BUG_ON(page_mapcount(page) > 1);

		// 4k page
		if(desc->huge_page == 0){
			/* move the old page to an isolated lru */
			err = isolate_lru_page(page);
			if (err) {
				// xzl -- EBUSY. this may happen when the page is still in
				// pagevec for batch processing
				E("iso page %pK phys %llx from lru failed. err %d",
						page, page_to_phys(page), err);

				BUG();
			}
			list_add_tail(&page->lru, &table->pagelist);
			inc_zone_page_state(page, NR_ISOLATED_ANON +
					page_is_file_cache(page));//??
		
		// 2M page
		}else{ 
			// how to deal with isolate huge page lru????
			/*
			
			add code here...

			*/

			if (PageHead(page)){
				isolate_huge_page(page, &table->pagelist);
			}
		}

		/*
		 * Either remove the duplicate refcount from
		 * isolate_lru_page() or drop the page ref if it was
		 * not isolated.
		 */
		put_page(page); //?? 

		
		/*
		 * Deal with the new page
		 */
		BUG_ON(!(desc->node == 0 || desc->node == 1));

		if(desc->huge_page == 0){ //4k page
			newpage = alloc_pages_exact_node(desc->node,
					GFP_HIGHUSER_MOVABLE | GFP_THISNODE, 0);
		}else{// 2M page

/*			if (PageHuge(page)){
				printk("PageHuge(page) true!\n");
			}else{
				printk("PageHuge(page) false\n");
			}
*/
			newpage = alloc_huge_page_node(page_hstate(compound_head(page)), 1);
			//printk("In migration, now alwayse allocate huge page from node 0!!! should change later !!!!!!\n");
			if(!newpage){
				printk("allocate huge page in kernel failed!!!\n");
				BUG_ON(!newpage);
			}else{
				//printk("allocate huge page in kernel success!!!\n");
			}
		}


		//printk("%s: %d\n", __func__, __LINE__);
		BUG_ON(IS_ERR(newpage));
		BUG_ON(!newpage);
		BUG_ON(page_mapcount(newpage) > 1);

		/* xzl: unmap_and_move() treats page_count(page) == 1 as a
		 * sign of ``old page was freed under us'' -- because it
		 * uses follow_page(POLL_GET)?
		 */
		// 4k page
		if(desc->huge_page == 0){
			if (unlikely(PageTransHuge(newpage)))
				BUG();	 /* unsupported */
		}

		/* from __unmap_and_move_ks2() */
		/* xzl: why lock the old page? protect against modification? */
		if (!trylock_page(page)) {
			// xzl: t&s the "locked" bit. ret (!old value). return 1 means locks
			// well.
			BUG();
			lock_page(page); /* this may sleep */
		}
/*		//hym: there is no cgroup in linux 4.4, so I comment these code
		mem_cgroup_prepare_migration(page, newpage, &table->pg_desc[j].mem);
		if (PageWriteback(page)) {
			BUG();
		}
*/
		//printk("%s: %d\n", __func__, __LINE__);

		if (PageAnon(page) && !PageKsm(page)) {
			anon_vma = page_get_anon_vma(page);
			if (!anon_vma)
				BUG();
		}

		if (unlikely(balloon_page_movable(page))) {
			BUG();
		}

		if (!page->mapping)
			BUG();

		/* we skip try_to_unmap(). instead, we use rmap_walk() to replace
		 * the old pte with the new one */
		if (!trylock_page(newpage))
			BUG();

		newpage->index = page->index;
		newpage->mapping = page->mapping;  /* xzl: this copies over anon_vma */

		if (PageSwapBacked(page))
			SetPageSwapBacked(newpage);
		BUG_ON(PageWriteback(page));	/* Writeback must be complete */

		V("before rmap walk newpage %pK count %d", newpage,
				page_count(newpage));

		//printk("%s: %d\n", __func__, __LINE__);

		/* We save them because
		 * 1. install_new_pte() will need @page
		 * 2. need to unlock @page/@newpage in dma callback */
		table->pg_desc[j].page = page;
		table->pg_desc[j].newpage = newpage;

		/* walk the anon_vma to update pte.
		 * this goes to install_new_pte() which will save
		 * the pte value and address to migtable->pg_desc.
		 *
		 * apparently, if has more than more mapcount, we need to save
		 * all ptep and pte on @table. */
		

		/***************** modified by hym*********************/
		//rmap_walk(newpage, install_new_pte, table->pg_desc + j);
		
		struct rmap_walk_control rwc = {
                	.rmap_one = install_new_pte,
                	.arg = table->pg_desc + j,
        	};

        	rmap_walk(newpage, &rwc);
		/**************** end by hym****************************/

		//printk("%s: %d\n", __func__, __LINE__);

		/* anon_vma no longer needed */
		if (anon_vma)
			put_anon_vma(anon_vma);

		table->pg_desc[j].status = 0;  /* more status code? */

		/*
		 * append to the sg lists
		 */
		BUG_ON((!sgs) || (!sgd));
		
#ifdef EMU_HUGETLB
		/* do dummy transfers. the dma engine driver won't
		 * care about page* anyway. */
		sg_set_page(sgs, 0, /* page* */ EMU_HUGETLB_PG_SIZE, 0);
		sg_set_page(sgd, 0, /* page* */EMU_HUGETLB_PG_SIZE, 0);

		sgs->dma_address = dummy_dma_src;
		sgd->dma_address = dummy_dma_dst;
#else		
		if(desc->huge_page == 0){ //4k page
			sg_set_page(sgs, opage[j], PAGE_SIZE, 0);
			sg_set_page(sgd, newpage, PAGE_SIZE, 0);
		}else{ //2M page
			sg_set_page(sgs, opage[j], HUGE_PAGE_SIZE, 0);
			sg_set_page(sgd, newpage, HUGE_PAGE_SIZE, 0);
		}

		//modified by hym
		//sgs->dma_address = phys40_to_dma32(page_to_phys(opage[j]));
		//sgd->dma_address = phys40_to_dma32(page_to_phys(newpage));
		// dma address = physical address on x86
		sgs->dma_address = page_to_phys(opage[j]);
		sgd->dma_address = page_to_phys(newpage);
		
		//hym: just set a random value, should change to the data size later...
		if(desc->huge_page == 0){
			sgs->dma_length = 4096;
		}else{
			sgs->dma_length = 2 * 1024 * 1024;
		}
		/****************************************/
#endif
		//printk("%s: %d\n", __func__, __LINE__);

		V("%d: phys_addr: src %llx (count %d) dst %llx (count %d) "
				"len %x", j,
				page_to_phys(opage[j]), page_count(opage[j]),
				page_to_phys(newpage), page_count(newpage),
				sg_dma_len(sgs));
/*		printk("%d: phys_addr: src %llx (count %d) dst %llx (count %d) "
				"len %x", j,
				page_to_phys(opage[j]), page_count(opage[j]),
				page_to_phys(newpage), page_count(newpage),
				sg_dma_len(sgs));
*/ //5.17
		prev_sgs = sgs;
		prev_sgd = sgd;

		sgs = sg_next(sgs);
		sgd = sg_next(sgd);
	}
	
	//printk("migrate-x86.c: %s line: %d\n", __func__, __LINE__); //5.17

	up_read(&mm->mmap_sem);

	kfree(opage);

	/* table setup */
	table->region = r;
	table->mm = mm;
	table->num_mig_descs = 1;
	table->num_pages = count;
	table->migdesc = desc;
	table->issue_one_desc_func = issue_one_desc_mig;
	/* XXX only necessary in proc context */
	table->user_cpu = smp_processor_id();

	BUG_ON(!prev_sgs || !prev_sgd);
	sg_mark_end(prev_sgs);
	sg_mark_end(prev_sgd);
	table->pg_desc[count].page = NULL; /* mark end */

	//printk("migrate-x86.c: %s line: %d\n", __func__, __LINE__); //5.17

	k2_measure("sglist-ready");

	if (do_irq)
		tx_flags |= DMA_PREP_INTERRUPT;

	//printk("migrate-x86.c: %s line: %d\n", __func__, __LINE__); //5.17

	/*
	 * Now we can use all NR_DEVICES channels.
	 *
	 * Add/modify code here if want to specify which channels we want to use
	 * especially, we want to use channels in diff controller: e.g. 0,8 channles
	 *
	 **/
	each_chan_page_count = count / NR_DEVICES; // using NR_DEVICES channels
	m_sgs = sg_src;
	m_sgd = sg_dst;

	for(i = 0; i < NR_DEVICES; i++){	
		//printk("**************** i = %d\n", i);
		sg_src_n[i] = m_sgs;
		sg_dst_n[i] = m_sgd;

		for(j = 0; j < each_chan_page_count; j++){
			m_sgs = sg_next(m_sgs);
			m_sgd = sg_next(m_sgd);
			//printk("   **************** j = %d\n", j);
		}
	}

	//static atomic_t my_counter = ATOMIC_INIT(4); //my_counter
	atomic_set(&my_counter, NR_DEVICES);

	test_flag = 0;

#if MULTI_CHAN_SERIAL
	sema_init(&sem, 1);
#endif
	
	for(i = 0; i < NR_DEVICES; i++){
#if MULTI_CHAN_SERIAL
		down(&sem);
#endif
		m_tx[i] = chans[i]->device->device_prep_dma_sg(chans[i], sg_dst_n[i], 
				each_chan_page_count, sg_src_n[i], each_chan_page_count, tx_flags);
		BUG_ON(!m_tx[i]);
		//BUG_ON(!tx);

		if (do_irq) {
			m_tx[i]->callback = dma_callback_kthread;
			//tx->callback = dma_callback_kthread;
			m_tx[i]->callback_param = (void *)table;
			//tx->callback_param = (void *)table;
		}

		//comment these temporally, remember to restore !!
		m_cookie[i] = dmaengine_submit(m_tx[i]);
		//cookie = dmaengine_submit(tx); // put to the chan's submitted list
		dma_async_issue_pending(chans[i]);
		//dma_async_issue_pending(info->chan); // kick the chan if it is not busy
		start_transfer[i] = ktime_get();
	}

	end = ktime_get();
	printk("config total time = %ld ns\n", ktime_to_ns(ktime_sub(end, start)));
	printk("start transfert time: ");
	for(i = 0; i < NR_DEVICES; i++){
		printk("%ld  ", ktime_to_ns(start_transfer[i]));
	}
	printk("\n");

	k2_measure("dma-issued");

	kfree(sg_src);

	//printk("func: %s, line: %d\n",__func__, __LINE__); //hym 5.17
	return table;
}

/*
 * empty the requested queue, change its color, and issue one desc.
 *
 * @issue_one_desc_func: the func that actually submits the desc.
 * can be @issue_one_desc_copy for replication (no remapping)
 * or @issue_one_desc_mig for migration (remapping) */
static int user_flush(mig_region *r, struct mm_struct *mm, int parent,
		issue_one_desc_t issue_one_desc_func)
{
	struct mig_table *table = NULL;
	mig_desc *desc = NULL;
	color_t c;

	I("user submits one desc");

	k2_measure("user-flush:start");

	/* empty the requested queue --> issued queue */
again:
	while ((desc = mig_dequeue(r, &(r->qreq)))) {
		mig_enqueue(r, &(r->qissued), desc);
	}
	c = migqueue_set_color_if_empty(r, &(r->qreq), COLOR_KER);
	if (c == -1) /* some descs enqueued under us */
		goto again;
	else if (c == COLOR_KER) /* some other users changed the color */
		return 0;

	V("color->ker");

	/* do one desc from the issued queue.
	 * we want the irq to happen so that kthread is scheduled. */

	desc = mig_dequeue(r, &(r->qissued));
	BUG_ON(!desc);

	/*
     * migrate_prep() needs to be called before we start compiling a list of
     * pages to be migrated using isolate_lru_page(). If scheduling work on
     * other CPUs is undesirable, use migrate_prep_local()
     *
     * xzl: this seems pretty expensive, taking ~35 us.
	 */
	migrate_prep();

	/* @table will be freed in softirq.
	 * @desc will be linked on @table  */
	table = issue_one_desc_func(r, desc, 1 /* irq yes */, mm);

	return 0;
}


/*
 * The generic kernel thread work scheduled by softirq.
 * Pushes the queued requests slowly to dma engine in small doses, sleep in between, and check later.
 *
 * Can do either copy or migration, based on @table->issue_one_desc_func.
 *
 * XXX we didn't grab mm
 */
static void kernel_flush_work(struct work_struct *work)
{
	color_t c;
	mig_desc *desc = NULL;
	struct mig_table *table = NULL;
	int i;

	struct mig_table *oldtable = \
			container_of(work, struct mig_table, work);
	mig_region *r = oldtable->region;
	struct mm_struct *mm = oldtable->mm;
	/* info@ is right after the mig_region. see if_open() */
	struct mmap_info *info = (struct mmap_info *) \
			((unsigned long)r + PAGE_SIZE * (NPAGES-1));

	k2_measure("kthread-enter");
	free_pages((unsigned long)oldtable, MIGTABLE_PAGE_ORDER);

	while (1) {
		int sleep_low, sleep_high;
		int repeat_low, repeat_high;

		desc = mig_dequeue(r, &(r->qissued));
		if (!desc) { /* issued queue empty */
			if (migqueue_color(r, &(r->qreq)) == COLOR_USR)
				/* don't touch */
				break;
			else
				desc = mig_dequeue(r, &(r->qreq));
		}
		if (!desc) { /* req queue empty -- try to alter its color */
			c = migqueue_set_color_if_empty(r, &(r->qreq), COLOR_USR);
			if (c == -1) /* something enters the req queue */
				continue;
			else {
				I("color->usr");
				k2_measure("kthread:ker->usr");
				k2_flush_measure_format();
				break;
			}
		}
		/* have a desc to work on */

		/* Submit the desc and issue the transfer.
		 *
		 * the func ptr can be either submit_one_desc() or
		 * issue_one_desc_mig().
		 */
		V("kthread issues one desc");
		BUG_ON(!oldtable->issue_one_desc_func);
		table = oldtable->issue_one_desc_func(r, desc, 0 /* no irq */, mm);

		/* Note that dma_async_issue_pending() has already
		   called by issue_one_desc_XXX funcs */

		/* how long should we sleep?
		 * 32 pages ~ 50 us
		 * 64 pages	~ 90 us
		 * 128 pages ~ 170 us
		 */
		switch (desc->order) {
		case 0: // 1 page -- perhaps we should spin wait.
			sleep_low = 10;
			sleep_high = 10;
			break;
		case 3: // 8 pages
			sleep_low = 20;
			sleep_high = 30;
			break;
		case 4: // 16 pages
			sleep_low = 30;
			sleep_high = 40;
			break;
		case 5:	// 32 pages
			sleep_low = 30;
			sleep_high = 50;
			break;
		case 6:	// 64 pages
			sleep_low = 70;
			sleep_high = 90;
			break;
		case 7:	// 128 pages
			sleep_low = 140;
			sleep_high = 170;
			break;
		default:
			sleep_low = 100;
			sleep_high = 150;
		}
		repeat_low = 30;
		repeat_high = 60;

#ifdef EMU_HUGETLB
		{
			/* scale the waiting vaules based on the 4kb-page wait time.
			 *
			 * 4kb page dma takes ~7us. */

			if (EMU_HUGETLB_PG_ORDER == 4) {
				/* 64kb page, each takes ~15 us */
				sleep_low = 15 * (1<<desc->order);
				sleep_high = 20 * (1<<desc->order);
			} else if (EMU_HUGETLB_PG_ORDER == 9) {
				if (desc->order == 4) {
					/* 2m page, each takes ~280 us.
					 * since the wire delay is dominant, we have to
					 * readjust based on desc->order */
					sleep_low = 250 * (1<<desc->order);
					sleep_high = 300 * (1<<desc->order);
				} else if (desc->order == 7) {
					sleep_low = 250 * (1<<desc->order);
					sleep_high = 350 * (1<<desc->order);
				}
				repeat_low = 250;
				repeat_high = 300;
			} else
				BUG();
		}
#endif

		for (i = 0; i < 10; i ++) {
			if (i == 0)
				usleep_range(sleep_low, sleep_high);
			else
				usleep_range(repeat_low, repeat_high); 	/* repeat check */
			k2_measure("kthread-wake");
			/* are we done yet? */
			
			//added by hym
			//if (info->chan->device->device_tx_status(info->chan, 0, NULL) == DMA_SUCCESS)
			// DMA_SUCCESS has been changed to DMA_COMPLETE
			if (info->chan->device->device_tx_status(info->chan, 0, NULL) == DMA_COMPLETE)
				break;
		}
		BUG_ON(i == 10);

		/* Clean up DMA.
		 * Should we do the following -- (from edma_callback())
		 * edma_stop(echan->ch_num)? */

		/* Okay: one desc completed */

		/* if we're doing migration, some cleanup work has to be done
		 * for each page. */
		if (table->issue_one_desc_func ==  issue_one_desc_mig) {
			for (i = 0; i < table->num_pages; i++)
				dma_complete_one_mig(table->pg_desc + i);
		}

		free_pages((unsigned long)table, MIGTABLE_PAGE_ORDER);
		mig_enqueue(r, &(r->qcomp), desc);
		k2_measure("kworker:enqueue-end");
		wake_up_interruptible(&info->wait_queue);
		V("kthread completes one desc");
	}
}

/* follow_page() is batched */
static int flush_single_qreq_noremap2(mig_region *r, int user,
		struct mm_struct *mm, int parent)
{
	struct mig_table *table = NULL;
	/* info@ is right after the mig_region. see if_open() */
	struct mmap_info *info = (struct mmap_info *) \
			((unsigned long)r + PAGE_SIZE * (NPAGES-1));

	struct scatterlist *sg_src = NULL, *sg_dst = NULL;
	struct scatterlist *sgd = NULL, *sgs = NULL, *prev_sgd = NULL, *prev_sgs = NULL;
	struct dma_async_tx_descriptor *tx = NULL;
	dma_cookie_t cookie;
	unsigned long tx_flags = 0;

	mig_desc 	*desc = NULL;
	int i, npages, j;
	unsigned long paddr, paddr1;
	color_t old_clr, new_clr, c;

	k2_measure("flush-start/nr");

	assert(r);
	assert(mm);

	if (user) {
		/* xzl: drain pagevec to LRU so that they can be
		 * isolated. expensive? */
//		migrate_prep();
		old_clr = COLOR_USR;
		new_clr = COLOR_KER;
	} else {
		old_clr = COLOR_KER;
		new_clr = COLOR_USR;
	}

	/* Flush one desc from req queue to issued queue.
	 * Start the reasonably small transfer with irq enabled.
	 *
	 * Change the queue color as needed.
	 */
	i = 0; /* # of descs */
	npages = 0;

	down_read(&mm->mmap_sem); /* neede for follow_page()? */
	while (1) {
		struct vm_area_struct *vma, *vma1;
		struct page **opage, **npage;
		int count;

//		if (!user)
//			usleep_range(100, 200);

		if (user)
			tx_flags |= DMA_PREP_INTERRUPT;

		desc = mig_dequeue(r, &(r->qreq));

		/*
		 * Done flushing the queue
		 * */
		if ((!desc)) {
			if (user || (!user && (i == 0))) {
				/* User done flushing the queue or kernel has flushed nothing
				 * from the queue. change the queue color.
				 */
				c = migqueue_set_color_if_empty(r, &(r->qreq),
						new_clr);
				if (c == -1) /* queue becomes non empty due to race. try again */
					continue;
				else { /* color changed okay */
					if (c != old_clr) {
						E("bug: color %d expected %d. i %d idx %d",
								c, old_clr, i, r->qreq.head.index);
						BUG();
					}
					if (user)
						I("user tx sets color: %s -> %s", color_str(old_clr),
							color_str(new_clr));
					else {
						I("tx %08x sets color: %s -> %s", parent, color_str(old_clr),
							color_str(new_clr));
						/* notify the user -- perhaps unnecessary */
						k2_measure("prior-wakeup/nr");
						if (!migqueue_is_empty(&r->qcomp))
							wake_up_interruptible(&info->wait_queue);
						k2_measure("clr:->usr/nr");
						k2_flush_measure_format();
					}
					break;
				}
			} else if (!user) {
				/* Kernel has done flushing something from the queue.
				 * Color remains COLOR_KER.
				 * Proceed to the xfer. */
				break;
			} else
				BUG();
		}

		/*
		 * We have a new desc to work on.
		 */
//		if (!user)
//			cond_resched();
		D("processing a desc...");

		/* do we have a fresh mig_table yet? */
		if (!table) {
			/* create & init a new mig table.
			 * we still need it to pass mm, mig_region between user and
			 * irq. */
			BUG_ON(sgs || sgd);
			if (sizeof(struct mig_table) >= \
					(1<<MIGTABLE_PAGE_ORDER) * PAGE_SIZE) {
				E("sizeof(struct mig_table) %d bytes > %lu",
						sizeof(struct mig_table), PAGE_SIZE);
				BUG();
			}

			table = (struct mig_table *)__get_free_pages(GFP_KERNEL,
					MIGTABLE_PAGE_ORDER);
			assert(table);
			V("mig_table sz %d max_nr_sg %d", sizeof(struct mig_table),
					MAX_NR_SG);
//			INIT_LIST_HEAD(&table->pagelist);

			/* assemble two sg lists -- we don't know how many pages we'll
			 * have yet. */
			sgs = sg_src = kmalloc(sizeof(struct scatterlist) * MAX_NR_SG,
								GFP_KERNEL);
			sgd = sg_dst = kmalloc(sizeof(struct scatterlist) * MAX_NR_SG,
								GFP_KERNEL);

			assert(sg_src && sg_dst);

			/* xzl -- this will be expensive */
			sg_init_table(sgd, MAX_NR_SG);
			sg_init_table(sgs, MAX_NR_SG);
		}

		/* -- set each page in the desc -- */
		count = (1 << desc->order);

		paddr = base_to_virt(desc->virt_base);
		paddr1 = base_to_virt(desc->virt_base_dest);
		vma = find_vma(mm, paddr);
		vma1 = find_vma(mm, paddr1);
		if (!vma || paddr < vma->vm_start || !vma_migratable(vma)) {
			E("vma for vaddr %lx does not look right", paddr);
			BUG();
		}
		if (!vma1 || paddr1 < vma1->vm_start || !vma_migratable(vma1)) {
			E("vma1 for vaddr %lx does not look right", paddr1);
			BUG();
		}
		D("paddr %08lx vma %pK paddr1 %08lx vma1 %pK",
				paddr, vma, paddr1, vma1);

		opage = kmalloc(sizeof(struct page *) * count, GFP_KERNEL);
		npage = kmalloc(sizeof(struct page *) * count, GFP_KERNEL);
		BUG_ON(!opage || !npage);

		// -- do we need to pass in FOLL_GET?
		follow_page_range(vma, paddr, count, opage, FOLL_SPLIT);
		follow_page_range(vma1, paddr1, count, npage, FOLL_SPLIT);

		for (j = 0; j < count; j++) {

			/* See do_move_page_to_node_array() for comments */
			BUG_ON(IS_ERR(opage[j]));
			if (!opage[j])
				BUG();
			BUG_ON(page_mapcount(opage[j]) > 1);

			BUG_ON(IS_ERR(npage[j]));
			/* this is possible, as the dest virt address range may not
			 * be populated with phys pages yet. */
			BUG_ON(!npage[j]);
			BUG_ON(page_mapcount(npage[j]) > 1);

			if (npages ++ == MAX_NR_SG) {
				/* over dma sglist size XXX */
				E("bug: npages %d too big for a sglist", npages);
				BUG();
			}

			/* append to the sg lists */
			BUG_ON((!sgs) || (!sgd));

			sg_set_page(sgs, opage[j], PAGE_SIZE, 0);
			sg_set_page(sgd, npage[j], PAGE_SIZE, 0);

			//sgs->dma_address = phys40_to_dma32(page_to_phys(opage[j]));
			//sgd->dma_address = phys40_to_dma32(page_to_phys(npage[j]));
			// modified by hym
			sgs->dma_address = page_to_phys(opage[j]);		
			sgd->dma_address = page_to_phys(npage[j]);	

			D("%d: phys_addr: src %llx (count %d) dst %llx (count %d) "
					"len %x", i,
					page_to_phys(opage[j]), page_count(opage[j]),
					page_to_phys(npage[j]), page_count(npage[j]),
					sg_dma_len(sgs));

			prev_sgs = sgs;
			prev_sgd = sgd;

			sgs = sg_next(sgs);
			sgd = sg_next(sgd);
		}

		kfree(opage);
		kfree(npage);

		mig_enqueue(r, &(r->qissued), desc);
		i ++;
	}
	k2_measure("pglist-ready/nr");

	if (i == 0) {
		/* no transfers to do. no matter whether we come from user or
		 * kernel thread, the previously got mm is not longer needed.
		 * so put it. otherwise, the mm will be leaked (held forever).
		 */
		BUG_ON(table);
		V("non desc read. mm %pK mmput()", mm);
		mmput(mm);
		goto out;
	}

	/* We have some descs. */
//	BUG_ON(list_empty(&table->pagelist));
	D("mm %pK total %d descs (%d pages) read from req queue", mm, i, npages);

	table->region = r;
	table->mm = mm;
	table->num_mig_descs = i;
	table->num_pages = npages;
	/* debug */
	if (user) {
		table->tx_id = atomic_inc_return(&usr_nr_tx);
		W("user starts tx %08x", table->tx_id);
	} else {
		table->tx_id = atomic_inc_return(&ker_nr_tx);
		W("tx %08x starts tx %08x", parent, table->tx_id);
	}

	BUG_ON(!chans[0]);

	/* finish the sg lists and submit */
	BUG_ON(!prev_sgs || !prev_sgd);
	sg_mark_end(prev_sgs);
	sg_mark_end(prev_sgd);

	table->pg_desc[npages].page = NULL; /* mark end */

	k2_measure("sglist-ready/nr");

	/* Thanks to IO coherency, no map/unmap needed */
	tx = chans[0]->device->device_prep_dma_sg(chans[0],
			sg_dst, npages, sg_src, npages,
			DMA_CTRL_ACK | DMA_PREP_INTERRUPT
			  | DMA_COMPL_SKIP_DEST_UNMAP | DMA_COMPL_SKIP_SRC_UNMAP
	);

	BUG_ON(!tx);
	k2_measure("dma-preped/nr");

	tx->callback = dma_callback_noremap;
	tx->callback_param = (void *)table;

	cookie = dmaengine_submit(tx); // put to the chan's submitted list
	dma_async_issue_pending(chans[0]); // kick the chan if it is not busy

	k2_measure("dma-issued/nr");

out:
	up_read(&mm->mmap_sem);
	k2_measure("flush-end/nr");

	kfree(sg_dst);
	kfree(sg_src);

	V("==== done ==== ");
	return i;
}

/* Try to get descs from a set of mig_regions, by flushing
 * their req queues.
 *
 * Called by kernel tasklet.
 *
 */
static int flush_multi_qreq(mig_region *rs, int count)
{
	return 0;
}

/* --------------- testers ----------------------- */

#ifndef VM_RESERVED
# define  VM_RESERVED   (VM_DONTEXPAND | VM_DONTDUMP)
#endif

#if 0
static void test_move(void)
{
	struct mm_struct *mm;
	struct task_struct *task;
	struct page_to_node pm[MAX_NR_SG + 1];	/* bad */
	struct mig_table *table;
	mig_desc 	*desc;
	int err, i, j;
	color_t oldc;

	task = current;
	mm = get_task_mm(task);
	assert(mm);

	assert(the_region);

#if 0
	for (i = 0; i < MAX_NR_SG; i++) {
		desc = mig_dequeue(the_region, &(the_region->qreq));
		if (!desc) {
			break;
		}
		pm[i].addr = (unsigned long)(desc->virt_base << MIG_BLOCK_ALIGN_OFFSET);
		pm[i].node = 1;

		mig_enqueue(the_region, &(the_region->qissued));
	}
#endif

	/* flush all descs from req queue to issued queue. assemble a migration
	 * array on the fly.
	 * when the req queue becomes empty, change its color so that the kernel
	 * will flush it in the future.
	 */
	i = 0;
	while (1) {
		desc = mig_dequeue(the_region, &(the_region->qreq));
		if (!desc) {
			oldc = migqueue_set_color_if_empty(the_region, &(the_region->qreq),
					COLOR_KER);
			if (oldc == -1) /* queue becomes non emtpy due to race. try again */
				continue;
			else {
				assert(oldc == COLOR_USR);
				break;
			}
		}
		pm[i].addr = (unsigned long)(desc->virt_base << MIG_BLOCK_ALIGN_OFFSET);
		pm[i].node = 1;

		mig_enqueue(the_region, &(the_region->qissued), desc);
		i ++;
		if (i == MAX_NR_SG) {
			/* over dma sglist size XXX */
			assert(0);
		}
	}

	pm[i].node = MAX_NUMNODES; /* end marker */

	if (i == 0) {
		W("non desc read. stop");
		return;
	} else
		W("%d descs read from queue", i);

	table = kmalloc(sizeof(struct mig_table), GFP_KERNEL);
	assert(table);
	I("going to move. mig_table size %d bytes vaddr %08lx node %d",
			sizeof(struct mig_table), pm[0].addr, pm[0].node);

	table->region = the_region;
	table->num_mig_descs = i;

	err = do_move_page_to_node_array(mm, pm, 1, table);
	I("moving is done. err %d status -- ", err);
	for (j = 0; j < i; j++)
		printk("%d ", pm[j].status);
	printk("\n");

	/* get stat */
//	pm[0].status = -1;
//	do_pages_stat_array(mm, 1, (const void **)(&(pm[0].addr)),
//			&(pm[0].status));
//	I("check status -- %d", pm[0].status);
}
#endif

static void test_fill_queue(mig_region *r)
{
	int cnt;
	mig_desc *desc;

	assert(r);

	cnt = 1;
	while ((desc = freelist_remove(r))) {
		desc->virt_base = cnt++; /* save the increasing counter */
		desc_get(desc);
		mig_enqueue(r, &(r->qreq), desc);

		if (cnt >= 8000)  /* exhausting all descs will fail deque() */
			break;
	}
	W("done. %d descs enqueued", cnt-1);
}

static void test_empty_queue(mig_region *r)
{
#define NCPUS 4

	int i, cnt, tid, sum;
	mig_desc *desc;
	int thread_cnts[NCPUS] = {0}; /* used in verification: emulate thread counters */

	/* --- verify --- */
	while ((desc = mig_dequeue(r, &(r->qreq)))) {
		tid = desc->flag;
		cnt = desc->virt_base;

		assert(tid < NCPUS);
		assert(cnt == thread_cnts[tid]);
		thread_cnts[tid] ++;

		desc_put(r, desc);
	}

	W("pass. all threads' counters look fine: ");
	sum = 0;
	for (i = 0; i < NCPUS; i++ ) {
		printk("%d ", thread_cnts[i]);
		sum += thread_cnts[i];
	}
	printk("sum: %d\n", sum);

#undef NCPUS
}

/* --------  the debugfs interface -------------- */

static struct dentry  *debugif[NR_DEVICES] = {NULL};

#if NR_DEVICES == 4
static const char * debug_fnames[NR_DEVICES] = \
		{"migif", "migif1", "migif2", "migif3"};
#endif

#if NR_DEVICES == 16
	static const char * debug_fnames[NR_DEVICES] = \
		{"migif", "migif1", "migif2", "migif3", \
		 "migif4", "migif5", "migif6", "migif7", \
		 "migif8", "migif9", "migif10", "migif11", \
		 "migif12", "migif13", "migif14", "migif15"};
#endif

static void mmap_open(struct vm_area_struct *vma)
{
    struct mmap_info *info = (struct mmap_info *)vma->vm_private_data;
    info->reference++;
}

static void mmap_close(struct vm_area_struct *vma)
{
    struct mmap_info *info = (struct mmap_info *)vma->vm_private_data;
    info->reference--;
}

static int mmap_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
    struct page *page = NULL;
    struct mmap_info *info;

    info = (struct mmap_info *)vma->vm_private_data;
    if (!info->region)
    {
        printk("No region allocated yet\n");
        return 0;
    }

    if (vmf->pgoff > NPAGES - 1) {
    	E("bug: out of migregion");
    	return -1;
    }

    page = virt_to_page((char *)info->region + (vmf->pgoff << PAGE_SHIFT));

    if (!page) {
    	E("failed to get phys page.");
    	return -1;
    }

    D("pgoff %lu", vmf->pgoff);

    get_page(page);
    vmf->page = page;

    return 0;
}

static struct vm_operations_struct mmap_vm_ops =
{
    .open =     mmap_open,
    .close =    mmap_close,
    .fault =    mmap_fault,
};


// xzl: this allocates pages on device file open. a good idea (or should we
// do it when mmap())?
int if_open(struct inode *inode, struct file *filp)
{
    char *p;
	struct mmap_info *info;
	int i, device_id = 0;

	/* XXX should improve this later */
	for (i = 0; i < NR_DEVICES; i++) {
		if (!strncmp(filp->f_path.dentry->d_iname,
				debug_fnames[i], 7)) {
			device_id = i;
			break;
		}
	}
	BUG_ON(i == NR_DEVICES);

    if (filp->private_data) {
    	E("double open() called. abort");
    	return -1;
    }

    /* these pages are for mig_region (N-1 pages) + device info (1 page).
     * so that later we can easily use mig_region* to find the corresponding
     * info* */
    p = (char *)__get_free_pages(GFP_KERNEL, NPAGEORDER);
    assert(p);

    info = (struct mmap_info *)(p + PAGE_SIZE * (NPAGES-1));
    info->region = (mig_region *)p;

    /* get a pre-allocated dma channel */
    /* when we are built in, the dma modules may be initialized later than us.
     * so do a check here.
     */
    if (!chans[device_id]) {
    	chans[device_id] = init_dma_chan();
    	BUG_ON(!chans[device_id]);
    }

    info->chan = chans[device_id];
    I("device_id %d chan_id %d", device_id, info->chan->chan_id);

    // xzl: populating the d/s in the allocated pages
    init_migregion(info->region, (NPAGES-1) * PAGE_SIZE);
    V("init migregion done. ndescs %d pages %d", info->region->ndescs, NPAGES-1);

    init_waitqueue_head(&info->wait_queue);
    sema_init(&info->sem, 1);

    /* assign this info struct to the file */
    filp->private_data = info;
    return 0;
}

static int if_mmap(struct file *filp, struct vm_area_struct *vma)
{
    vma->vm_ops = &mmap_vm_ops;
    vma->vm_flags |= VM_RESERVED;
    vma->vm_private_data = filp->private_data;
    mmap_open(vma);
    return 0;
}

static int if_close(struct inode *inode, struct file *filp)
{
    /* According to ldd3 ch15, when vma is unmapped, the kernel will
     * dec page refcnt which will automatically put the page to freelist. (?)
     * thus, manual freeing pages seem to double free the pages and thus
     * trigger bad_page().
     *
     * This seems problematic. ldd3 says higher order allocation needs special
     * care but didn't explain how.
     * (Need to understand compound page count better)
     *
     * https://groups.google.com/forum/#!topic/comp.os.linux.development.system/xCwYMMvjgCc
     * (Should reset page count?)
     *
     * So let them leak now...
     */
//    free_pages((unsigned long)info->data, NPAGEORDER);

    /* we don't have to free @info, as it was allocated together with
     * migregion in a shot.
     */
    filp->private_data = NULL;
    D("migregion+info freed");
    return 0;
}

#if 1
static ssize_t if_write(struct file *f, const char __user *u,
		size_t sz, loff_t *off)
{
	int cmd = -1;
	struct mmap_info *info = f->private_data;
	struct mm_struct *mm;

	if (sz < 4)
		return -EFAULT;

	if(copy_from_user(&cmd, u, sizeof(int)))
		return -EFAULT;

	D("info %08x cmd %d", (int32_t)info, cmd);

	switch (cmd) {
	case MIG_INIT_REGION:
		/* XXX -- grab a lock here!
		 * re init the region (with the allocated buffer) */
		init_migregion(info->region, (NPAGES-1) * PAGE_SIZE);
		k2_measure_clean();
		D("init migregion done. ndescs %d", info->region->ndescs);
#if 0
		/* debugging */
		if (lastpage) {
			E("lastpage %pK. PageLRU %d PageUnevictable %d "
					"Active %d count %d mapcount %d",
					lastpage,
					PageLRU(lastpage), PageUnevictable(lastpage),
					PageActive(lastpage),
					page_count(lastpage), page_mapcount(lastpage));
			check_vma(lastpage);
		}
#endif
		break;
	case MIG_FILL_QREQ:
		test_fill_queue(info->region);
		break;
	case MIG_EMPTY_QREQ:
		test_empty_queue(info->region);
		break;
	case MIG_MOVE_SINGLE:
		/* xzl: need get_task_struct() / put_task_struct()?
		 * this increases mm->mm_users. we'll put it in callback (so that
		 * the mm won't go away)
		 * */
		mm = get_task_mm(current);
		flush_single_qreq(info->region, 1, mm, -1);
		break;
	case MIG_MOVE_SINGLE_NOREMAP:
		mm = get_task_mm(current);
//		flush_single_qreq_noremap2(info->region, 1, mm, -1);
		printk("MIG_MOVE_SINGLE_NORMAP ------\n");
		user_flush(info->region, mm, -1, issue_one_desc_copy);
		mmput(mm); /* XXX good? */
		break;
	case MIG_MOVE_SINGLE_MIG:
		printk("MIG_MOVE_SINGLE_MIG ++++++++\n");
		mm = get_task_mm(current);
		user_flush(info->region, mm, -1, issue_one_desc_mig);
		mmput(mm);
		break;
	default:
		W("unknown cmd %d", cmd);
	}

	return sz;
}
#endif

/* what's wrong with this? cmd always reads 0; user always gets return -1 */
static long if_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
#if 0
	struct mmap_info *info = f->private_data;

	I("info %08x cmd %d", (int32_t)info, cmd);

	switch (cmd) {
	default:
		W("unknown cmd %u", cmd);
		break;
	}
#endif
	return 0;
}

static unsigned int if_poll(struct file *filp, poll_table *wait)
{
	struct mmap_info *info = filp->private_data;
	unsigned int mask = 0;

	/* xzl -- need to grab a sem? */
	down(&info->sem);
	poll_wait(filp, &info->wait_queue,  wait);
	up(&info->sem);

	if (!migqueue_is_empty(&info->region->qcomp)) {
		D("qcomp is not empty.");
		mask |= (POLLIN | POLLRDNORM);	/* readable */
	}

	return mask;
}

static const struct file_operations mmap_fops = {
    .open 		= 	if_open,
    .release 	= 	if_close,
    .mmap 		= 	if_mmap,
    .write 		=	if_write,
    .poll 		= 	if_poll,
    .unlocked_ioctl = if_ioctl,
};


/* --------  module entry / exit -------------- */

static int __init migks2_module_init(void)
{
	int i;
	for (i = 0; i < NR_DEVICES; i++) {
		debugif[i] = debugfs_create_file(debug_fnames[i], 0644,
				NULL, NULL, &mmap_fops);
		BUG_ON(!debugif[i]);

		/* we want to init all dma chans and keep them alive
		 * through the module lifetime. */
		chans[i] = init_dma_chan();
	}

#ifdef EMU_HUGETLB
	E("warning -- EMU_HUGETLB is on. DMA uses dummy buffers (order %d)",
			EMU_HUGETLB_PG_ORDER);

	the_ddr_pages = alloc_pages_exact_node(0,
			GFP_HIGHUSER_MOVABLE | GFP_THISNODE, EMU_HUGETLB_PG_ORDER);
	assert(the_ddr_pages);
	
	// mofidied by hym
	//the_dummy_dma[0] = phys40_to_dma32(page_to_phys(the_ddr_pages));
	the_dummy_dma[0] = page_to_phys(the_ddr_pages);
	the_dummy_dma[1] = KEYSTONE_MSMC_PHYS_START;
#endif

    return 0;
}

static void __exit migks2_module_exit(void)
{
	int i;
	for (i = 0; i < NR_DEVICES; i++) {
		if (debugif[i])
			debugfs_remove(debugif[i]);
		if (chans[i])
			dma_release_channel(chans[i]);
	}

#ifdef EMU_HUGETLB
	__free_pages(the_ddr_pages, EMU_HUGETLB_PG_ORDER);
#endif

	k2_measure_clean();
}

module_init(migks2_module_init);
module_exit(migks2_module_exit);
MODULE_LICENSE("GPL");

