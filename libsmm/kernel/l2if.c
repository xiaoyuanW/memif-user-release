/*
 * Testing ARM's l2 prefetch function.
 *
 * xzl, for memif project. 2015.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <asm/uaccess.h>

/* ref:
 * http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.ddi0438c/BABEHIAI.html
 *
 * Seems it is readonly in our mode.
 */
#include "log-xzl.h"

static struct dentry  *file;

static inline void write_reg(unsigned int value)
{
	unsigned int oldv = 0xdeadbeef;
	asm volatile ("MRC p15, 1, %0, c15, c0, 3\t\n": "=r"(oldv));
	I("read, reg=%08x", oldv);

	// MCR p15, 1, <Rt>, c15, c0, 3; Write L2 Prefetch Control Register
	asm volatile ("MCR p15, 1, %0, c15, c0, 3\t\n" : : "r"(value));

	asm volatile ("MRC p15, 1, %0, c15, c0, 3\t\n": "=r"(oldv));
	I("read again, reg=%08x", oldv);
}

static inline unsigned int read_reg(void)
{
	unsigned int value = 0xdeadbeef;
	// MRC p15, 1, <Rt>, c15, c0, 3; Read L2 Prefetch Control Register
	asm volatile ("MRC p15, 1, %0, c15, c0, 3\t\n": "=r"(value));

	return value;
}

static ssize_t f_test_write(struct file *f, const char __user *u,
		size_t sz, loff_t *off)
{
	unsigned int cmd = 0xdeadbeef;

	if (sz > 20)	/* too large? */
		return -EFAULT;

	if (kstrtouint_from_user(u, sz, 0, &cmd)) {
		W("failed to convert sz %d. cmd %08x", sz, cmd);
		return -EFAULT;
	}

	I("to write regvalue = %08x", cmd);
	write_reg(cmd);

	return sz;
}

static ssize_t f_test_read(struct file *f, char __user * u, size_t sz,
		loff_t *off)
{
	unsigned v;
	int len;

	if (*off > 0)
		return 0;

	if (sz < 20)
		return -EFAULT;

	v = read_reg();
	len = snprintf(u, sz, "%08x\n", v);

	*off += len;
	return len;
}


static const struct file_operations test_fops = {
    .write = f_test_write,
    .read = f_test_read,
};

static int __init mmapexample_module_init(void)
{
    file = debugfs_create_file("l2prefetch", 0644, NULL, NULL, &test_fops);
    return 0;
}

static void __exit mmapexample_module_exit(void)
{
    debugfs_remove(file);
}

module_init(mmapexample_module_init);
module_exit(mmapexample_module_exit);
MODULE_LICENSE("GPL");
