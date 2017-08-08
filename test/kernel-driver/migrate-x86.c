#include <linux/module.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include "mig.h"
#include "log-xzl.h"

#define NR_DEVICES 4
#define NPAGEORDER 2
#define NPAGES (1 << NPAGEORDER)


static struct dentry *debugif[NR_DEVICES] = {NULL};
static const char * debug_fnames[NR_DEVICES] = \
		{"migif", "migif1", "migif2", "migif3"};


//test: kernel fill the queue
static void test_fill_queue(mig_region *r){
	int cnt;
	mig_desc *desc;

	assert(r);

	cnt = 1;
	while((desc = freelist_remove(r))){
		desc->virt_base = cnt ++;
		desc_get(desc);
		mig_enqueue(r, &(r->qreq), desc);

		if(cnt > 100){ /* exhausting all descs will fail deque() */
			break;	//but there only 379 in fact????
		}
		printk("kernel wait!\n");
	}
	printk("test_fill_queue done!\n");
}

// test: kenel fill the queue all the time
static void test_fill_queue_always(mig_region *r){
	
}


/* per file d/s. this is pointed by filp->private_data.
 * in allocating it, we give this d/s one page right after the file's
 * @mig_region. */
struct mmap_info{
	//    char *data;       // xzl: points to the kernel pages that back mig_region
	struct dma_chan *chan;
	mig_region *region;
	int reference;
	wait_queue_head_t wait_queue;
	struct semaphore sem;  /* for fd protection */
};


//vm_operations_struct mmap_vm_ops
static void mmap_open(struct vm_area_struct *vma){
	printk("%s %d\n", __func__, __LINE__);
	struct mmap_info *info = (struct mmap_info *)vma->vm_private_data;
	info->reference++;
	printk("%s %d\n", __func__, __LINE__);
}

static void mmap_close(struct vm_area_struct *vma){
	struct mmap_info *info = (struct mmap_info *)vma->vm_private_data;
	info->reference--;
}

static int mmap_fault(struct vm_area_struct *vma, struct vm_fault *vmf){
	struct page *page = NULL;
	struct mmap_info *info;
	
	printk("%s %d\n", __func__, __LINE__);
	//printk("+++++++++++++++++++++++++++++++++++++++++++++\n");
	info = (struct mmap_info *)vma->vm_private_data;
	if (!info->region){
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

	printk("%s %d\n", __func__, __LINE__);

	return 0;
}

static struct vm_operations_struct mmap_vm_ops = {
	.open	= mmap_open,
	.close	= mmap_close,
	.fault	= mmap_fault,
};

// file_operations mmap_fops if_*...
int if_open(struct inode *inode, struct file *filp){
	
	/*
	 *  add code here
	 */

	char *p;
	int i;
	mig_region * mig_region_k;
	struct mmap_info *info;

	printk(KERN_INFO "enter if_open!\n");
	for(i = 0; i < NR_DEVICES; i++){
		if(!strncmp(filp->f_path.dentry->d_iname,
				debug_fnames[i], 7)){
			break;		
		}
	}

	if (filp->private_data) {
		printk(KERN_INFO "double open() called. abort\n");
		return -1;
	}

	/* these pages are form mig_region (N-1 pages) + devices info (1 page)
	 * so that later we can easily use mig_region* to fin the conrresponding 
	 * info* 
	 */
	p = (char *)__get_free_pages(GFP_KERNEL, NPAGEORDER);
	if(!p){
		printk(KERN_INFO "get_free_pages failed!\n");
		return -1;
	}

	info = (struct mmap_info *)(p + PAGE_SIZE * (NPAGES-1));
	info->region = (mig_region *)p;

	/*
	*  channel here
	*/

	mig_region_k = (mig_region *)p;
	init_migregion(mig_region_k, (NPAGES-1) * PAGE_SIZE);
	printk(KERN_INFO "mig_region_k->ndescs = %d\n", mig_region_k->ndescs);
 
 	/* assign this info struct to the file */
	filp->private_data = info;

	return 0;
}

static int if_close(struct inode *inode, struct file *filp){
	/*
	*  add code here
	*/

	return 0;
}

static ssize_t if_write(struct file *f, const char __user *u,
		size_t sz, loff_t *off){
	/*
	 * add code here
	 */
	int cmd = -1;
	struct mmap_info *info = f->private_data;

	if(sz < 4){
		return -EFAULT;
	}

	if(copy_from_user(&cmd, u, sizeof(int))){
		return -EFAULT;
	}
	printk("%s cmd: %d\n", __func__, cmd);

	switch(cmd){
		case MIG_INIT_REGION:
			init_migregion(info->region, (NPAGES - 1) * PAGE_SIZE);
			printk("init migregion done. ndescs %d\n", info->region->ndescs);
			break;
		
		case MIG_FILL_QREQ:
			test_fill_queue(info->region);
			break;
		/*
		* add other case here
		*/

		default:
			printk("unknown cmd %d\n", cmd);
	}
	
	return sz;
}

static int if_mmap(struct file *filp, struct vm_area_struct *vma){
	
	printk("enter if_mmap!\n");
	
	vma->vm_ops = &mmap_vm_ops;
	//vma->vm_flags = VM_RESERVED; //not support anymore after linux 3.7
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_private_data = filp->private_data;
	mmap_open(vma);
	
	printk("%s %d\n", __func__, __LINE__);
	return 0;
}

static const struct file_operations mmap_fops = {
	.open		= if_open,
	.mmap		= if_mmap,
	.write		= if_write,
	.release	= if_close,
};


static int __init migx86_module_init(void){
	int i;
	printk(KERN_INFO "enter migx86_module_init\n");
	
	for(i = 0; i < NR_DEVICES; i++){
		debugif[i] = debugfs_create_file(debug_fnames[i], 0644,
			NULL, NULL, &mmap_fops);
		if(!debugif[i]){
			printk(KERN_INFO "debugfs_create_file failed!!!\n");
		}else{
			printk(KERN_INFO "debugfs_create_file success!\n");
		}
	}

	return 0;
}

static void __exit migx86_module_exit(void){
	int i;
	
	for(i = 0; i < NR_DEVICES; i++){
		if(debugif[i]){
			debugfs_remove(debugif[i]);
		}
	}
	
	printk(KERN_INFO "exit migx86_module_exit\n");
}

module_init(migx86_module_init);
module_exit(migx86_module_exit);

MODULE_LICENSE("GPL");
