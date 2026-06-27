/*
 * PS5 BAR2 Dump Module (standandable, loadable on running kernel)
 *
 * Maps the spcie device's BAR2 (2MB) and exports it via debugfs.
 * Also exports an ioctl on a misc device for userspace access.
 *
 * The 2MB BAR2 region may be the serial flash window.
 * EMC firmware is at offset 0x4000, size 0x7E000.
 *
 * Build: make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
 * Load:  insmod ps5_bar2_dump.ko
 * Read:  dd if=/sys/kernel/debug/ps5_bar2_dump of=bar2.bin bs=4096
 *   or:  use the /dev/ps5_bar2 misc device with read()
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/debugfs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/fs.h>

#define BAR2_SIZE (2 * 1024 * 1024) /* 2MB */
#define SONY_VID 0x104d
#define SPCIE_DID 0x9107

static void __iomem *bar2_base;
static struct dentry *debugfs_entry;
static struct pci_dev *grabbed_pdev;

static ssize_t bar2_read_file(struct file *filp, char __user *buf,
			      size_t count, loff_t *ppos)
{
	loff_t pos = *ppos;
	size_t to_read;
	u8 *tmp;
	ssize_t ret = 0;

	if (pos >= BAR2_SIZE)
		return 0;
	if (pos + count > BAR2_SIZE)
		count = BAR2_SIZE - pos;
	to_read = count;

	tmp = kmalloc(min_t(size_t, to_read, PAGE_SIZE), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	while (to_read > 0) {
		size_t chunk = min_t(size_t, to_read, PAGE_SIZE);
		memcpy_fromio(tmp, bar2_base + pos, chunk);
		if (copy_to_user(buf + ret, tmp, chunk)) {
			ret = -EFAULT;
			break;
		}
		ret += chunk;
		pos += chunk;
		to_read -= chunk;
	}

	kfree(tmp);
	if (ret > 0)
		*ppos = pos;
	return ret;
}

static const struct file_operations bar2_fops = {
	.owner = THIS_MODULE,
	.read = bar2_read_file,
	.llseek = default_llseek,
};

static struct miscdevice bar2_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ps5_bar2",
	.fops = &bar2_fops,
};

static int __init ps5_bar2_init(void)
{
	struct pci_dev *pdev;
	struct resource *res;
	int ret;

	pdev = pci_get_device(SONY_VID, SPCIE_DID, NULL);
	if (!pdev) {
		pr_err("ps5_bar2: spcie device not found\n");
		return -ENODEV;
	}

	/* BAR2 = region index 2 */
	res = &pdev->resource[2];
	if (!res->start || resource_size(res) < BAR2_SIZE) {
		pr_err("ps5_bar2: BAR2 not available (start=%llx size=%llx)\n",
		       (u64)res->start, (u64)resource_size(res));
		pci_dev_put(pdev);
		return -ENODEV;
	}

	if (!request_mem_region(res->start, BAR2_SIZE, "ps5_bar2")) {
		pr_err("ps5_bar2: request_mem_region failed\n");
		pci_dev_put(pdev);
		return -EBUSY;
	}

	bar2_base = ioremap(res->start, BAR2_SIZE);
	if (!bar2_base) {
		pr_err("ps5_bar2: ioremap failed\n");
		release_mem_region(res->start, BAR2_SIZE);
		pci_dev_put(pdev);
		return -ENOMEM;
	}

	/* Read first 16 bytes to verify it's accessible */
	{
		u8 first16[16];
		memcpy_fromio(first16, bar2_base, 16);
		pr_info("ps5_bar2: first 16 bytes: %*phN\n", 16, first16);
	}

	debugfs_entry = debugfs_create_file("ps5_bar2_dump", 0444, NULL,
					    NULL, &bar2_fops);

	ret = misc_register(&bar2_misc);
	if (ret) {
		pr_err("ps5_bar2: misc_register failed: %d\n", ret);
		debugfs_remove(debugfs_entry);
		iounmap(bar2_base);
		release_mem_region(res->start, BAR2_SIZE);
		pci_dev_put(pdev);
		return ret;
	}

	grabbed_pdev = pdev; /* keep reference */
	pr_info("ps5_bar2: 2MB BAR2 mapped at %px (phys %llx)\n",
		bar2_base, (u64)res->start);
	pr_info("ps5_bar2: read via /dev/ps5_bar2 or /sys/kernel/debug/ps5_bar2_dump\n");
	return 0;
}

static void __exit ps5_bar2_exit(void)
{
	misc_deregister(&bar2_misc);
	debugfs_remove(debugfs_entry);
	if (bar2_base) {
		iounmap(bar2_base);
		if (grabbed_pdev)
			release_mem_region(grabbed_pdev->resource[2].start,
					   BAR2_SIZE);
	}
	if (grabbed_pdev)
		pci_dev_put(grabbed_pdev);
	pr_info("ps5_bar2: unloaded\n");
}

module_init(ps5_bar2_init);
module_exit(ps5_bar2_exit);

MODULE_AUTHOR("PS5 Linux");
MODULE_DESCRIPTION("PS5 BAR2 (serial flash) dump module");
MODULE_LICENSE("GPL");
