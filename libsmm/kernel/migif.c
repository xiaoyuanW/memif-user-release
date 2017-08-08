/*
 *  migif.c
 *
 *  Created on: Jul 1, 2015
 *      Author: xzl
 *
 *  Use the memif kernel interface to test usr/kernel queues.
 *
 *  ref: http://people.ee.ethz.ch/~arkeller/linux/code/mmap_bart_tanghe_dan_hordern.c
 *  http://www.makelinux.net/ldd3/chp-15-sect-2
 *  http://stackoverflow.com/questions/10760479/mmap-kernel-buffer-to-user-space
 */


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <asm/uaccess.h>

#include "log-xzl.h"
#include "mig.h"

#ifndef VM_RESERVED
# define  VM_RESERVED   (VM_DONTEXPAND | VM_DONTDUMP)
#endif

#define NPAGEORDER 5
#define NPAGES (1 << NPAGEORDER)
// memory overlay for the shared pages
static mig_region * region;

static struct dentry  *file;
static struct dentry  *f_test;	// for testing

struct mmap_info
{
    char *data;	// xzl: points to the kernel vaddr of the allocated page
    int reference;
};

static struct mmap_info *ginfo = NULL; 	// xzl: dirty. XXX we only has one info.

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
    if (!info->data)
    {
        printk("No data\n");
        return 0;
    }

    page = virt_to_page(info->data + (vmf->pgoff << PAGE_SHIFT));

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

static int op_mmap(struct file *filp, struct vm_area_struct *vma)
{
    vma->vm_ops = &mmap_vm_ops;
    vma->vm_flags |= VM_RESERVED;
    vma->vm_private_data = filp->private_data;
    mmap_open(vma);
    return 0;
}

static int mmapfop_close(struct inode *inode, struct file *filp)
{
    struct mmap_info *info = filp->private_data;

    /* according to ldd3 ch15, when vma is unmapped, the kernel will
     * dec page refcnt which will automatically put the page to freelist. (?)
     * thus, manual freeing pages seems to double free the pages and thus
     * trigger bad_page().
     *
     * This seems problematic. ldd3 says higher order allocation needs special
     * care but didn't explain how.
     * (Need to understand page count better)
     *
     * https://groups.google.com/forum/#!topic/comp.os.linux.development.system/xCwYMMvjgCc
     * (Should reset page count?)
     */
//    free_pages((unsigned long)info->data, NPAGEORDER);
    kfree(info);
    filp->private_data = NULL;
    I("page freed");
    return 0;
}

/* --------------- testers ----------------------- */

static void test_fill_queue(void)
{
	int cnt;
	mig_desc *desc;

	assert(region);

	cnt = 1;
	while ((desc = freelist_remove(region))) {
		desc->virt_base = cnt++; /* save the increasing counter */
		desc_get(desc);
		mig_enqueue(region, &(region->qreq), desc);
	}
	W("done. %d descs enqueued", cnt-1);
}

static void test_empty_queue(void)
{
#define NCPUS 4

	int i, cnt, tid, sum;
	mig_desc *desc;
	int thread_cnts[NCPUS] = {0}; /* used in verification: emulate thread counters */

	/* --- verify --- */
	while ((desc = mig_dequeue(region, &(region->qreq)))) {
		tid = desc->flag;
		cnt = desc->virt_base;

		assert(tid < NCPUS);
		assert(cnt == thread_cnts[tid]);
		thread_cnts[tid] ++;

		desc_put(region, desc);
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

// XXX: use a kernel thread to contend with the userspace.
// ref: http://stackoverflow.com/questions/5280693/kernel-thread-implementation
// http://www.programering.com/a/MDN4IjMwATk.html
static int kthread_enq_deq(void *data)
{
	return 0;
}

/* test i/f */
static ssize_t f_test_write(struct file *f, const char __user *u,
		size_t sz, loff_t *off)
{
	int cmd = -1;

	if (sz < 4)
		return -EFAULT;

	if(copy_from_user(&cmd, u, sizeof(int)))
		return -EFAULT;

	switch (cmd) {
	case MIG_INIT_REGION:
		assert(ginfo->data);
		/* re init the region (with the allocated buffer) */
		region = init_migregion(ginfo->data, NPAGES * PAGE_SIZE);
		I("init migregion done. ndescs %d", region->ndescs);
		break;
	case MIG_FILL_QREQ:
		test_fill_queue();
		break;
	case MIG_EMPTY_QREQ:
		test_empty_queue();
		break;
	default:
		W("unknown cmd %d", cmd);
	}

	return sz;
}

/* -------------------------------------------- */

// xzl: this allocates pages on device file open. a good idea (or should we
// do it when mmap())?
int mmapfop_open(struct inode *inode, struct file *filp)
{
    struct mmap_info *info = kmalloc(sizeof(struct mmap_info), GFP_KERNEL);

    info->data = (char *)__get_free_pages(GFP_KERNEL, NPAGEORDER);	// xzl: 4 pages
    assert(info->data);
    ginfo = info;

    // xzl: populating the d/s in the allocated pages
    region = init_migregion(info->data, NPAGES * PAGE_SIZE);
    I("init migregion done. ndescs %d", region->ndescs);

    /* assign this info struct to the file */
    filp->private_data = info;
    return 0;
}

static const struct file_operations mmap_fops = {
    .open = mmapfop_open,
    .release = mmapfop_close,
    .mmap = op_mmap,
};

static const struct file_operations test_fops = {
    .write = f_test_write,
};

static int __init mmapexample_module_init(void)
{
    file = debugfs_create_file("migif", 0644, NULL, NULL, &mmap_fops);
    f_test = debugfs_create_file("migtest", 0644, NULL, NULL, &test_fops);
    return 0;
}

static void __exit mmapexample_module_exit(void)
{
    debugfs_remove(file);
    debugfs_remove(f_test);
}

module_init(mmapexample_module_init);
module_exit(mmapexample_module_exit);
MODULE_LICENSE("GPL");
