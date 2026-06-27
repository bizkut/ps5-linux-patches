/*
 * PS5 ICC Raw Query + BAR Dump Module (standalone, loadable)
 *
 * Uses the existing icc_query() exported symbol from spcie driver.
 * Provides /dev/ps5_icc_raw for sending raw ICC queries from userspace.
 * Also provides /dev/ps5_bar2 (2MB spcie BAR2) and /dev/ps5_emc_ddr (16MB).
 *
 * Build: make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
 * Load:  insmod ps5_bar2_dump.ko
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
#include <linux/ps5.h>

#define SONY_VID 0x104d
#define SPCIE_DID 0x9107
#define TPCIE_DID 0x90ec

#define SPCIE_BAR2_SIZE (2 * 1024 * 1024)
#define EMC_DDR_DUMP_SIZE (16 * 1024 * 1024)

/* ioctl definitions — must match userspace */
struct icc_raw_query {
	u32 query_len;
	u32 reply_len;
	u64 data;
};

struct icc_bar2_read {
	u32 offset;
	u32 size;
	u64 data;
};

#define ICC_IOC_MAGIC 'I'
#define ICC_RAW_QUERY  _IOWR(ICC_IOC_MAGIC, 2, struct icc_raw_query)
#define ICC_BAR2_READ  _IOWR(ICC_IOC_MAGIC, 3, struct icc_bar2_read)

/* Use the existing icc_query from spcie driver */
extern int icc_query(u8 *query, u8 *reply);

static void __iomem *spcie_bar2;
static void __iomem *emc_ddr_base;
static struct pci_dev *spcie_pdev;
static struct pci_dev *tpcie_pdev;

static ssize_t mmio_read(struct file *filp, char __user *buf,
			 size_t count, loff_t *ppos,
			 void __iomem *base, size_t region_size)
{
	loff_t pos = *ppos;
	size_t to_read;
	u8 *tmp;
	ssize_t ret = 0;

	if (pos >= region_size)
		return 0;
	if (pos + count > region_size)
		count = region_size - pos;
	to_read = count;

	tmp = kmalloc(min_t(size_t, to_read, PAGE_SIZE), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	while (to_read > 0) {
		size_t chunk = min_t(size_t, to_read, PAGE_SIZE);
		memcpy_fromio(tmp, base + pos, chunk);
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

static ssize_t spcie_bar2_fread(struct file *filp, char __user *buf,
				size_t count, loff_t *ppos)
{
	return mmio_read(filp, buf, count, ppos, spcie_bar2, SPCIE_BAR2_SIZE);
}

static const struct file_operations spcie_bar2_fops = {
	.owner = THIS_MODULE,
	.read = spcie_bar2_fread,
	.llseek = default_llseek,
};

static struct miscdevice spcie_bar2_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ps5_bar2",
	.fops = &spcie_bar2_fops,
};

static ssize_t emc_ddr_fread(struct file *filp, char __user *buf,
			     size_t count, loff_t *ppos)
{
	return mmio_read(filp, buf, count, ppos, emc_ddr_base, EMC_DDR_DUMP_SIZE);
}

static const struct file_operations emc_ddr_fops = {
	.owner = THIS_MODULE,
	.read = emc_ddr_fread,
	.llseek = default_llseek,
};

static struct miscdevice emc_ddr_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ps5_emc_ddr",
	.fops = &emc_ddr_fops,
};

static long icc_raw_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case ICC_RAW_QUERY: {
		struct icc_raw_query rq;
		u8 *buf;
		int ret;

		if (copy_from_user(&rq, (void __user *)arg, sizeof(rq)))
			return -EFAULT;
		if (rq.query_len < ICC_MSG_MIN_SIZE || rq.query_len > ICC_MSG_MAX_SIZE)
			return -EINVAL;
		if (rq.reply_len < ICC_MSG_MIN_SIZE || rq.reply_len > ICC_MSG_MAX_SIZE)
			return -EINVAL;

		buf = kmalloc(max(rq.query_len, rq.reply_len), GFP_KERNEL);
		if (!buf)
			return -ENOMEM;

		if (copy_from_user(buf, (void __user *)rq.data, rq.query_len)) {
			kfree(buf);
			return -EFAULT;
		}

		ret = icc_query(buf, buf);
		if (ret) {
			kfree(buf);
			return ret;
		}

		{
			struct icc_msg *msg = (struct icc_msg *)buf;
			u32 actual = msg->length;
			if (actual > rq.reply_len)
				actual = rq.reply_len;
			rq.reply_len = actual;
			if (copy_to_user((void __user *)arg, &rq, sizeof(rq))) {
				kfree(buf);
				return -EFAULT;
			}
			if (copy_to_user((void __user *)rq.data, buf, actual)) {
				kfree(buf);
				return -EFAULT;
			}
		}

		kfree(buf);
		return 0;
	}
	case ICC_BAR2_READ: {
		struct icc_bar2_read br;
		u8 *buf;

		if (copy_from_user(&br, (void __user *)arg, sizeof(br)))
			return -EFAULT;
		if (br.size == 0 || br.size > 4096)
			return -EINVAL;
		if ((u64)br.offset + br.size > SPCIE_BAR2_SIZE)
			return -EINVAL;
		if (!spcie_bar2)
			return -ENODEV;

		buf = kmalloc(br.size, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		memcpy_fromio(buf, spcie_bar2 + br.offset, br.size);
		if (copy_to_user((void __user *)br.data, buf, br.size)) {
			kfree(buf);
			return -EFAULT;
		}
		kfree(buf);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations icc_raw_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = icc_raw_ioctl,
};

static struct miscdevice icc_raw_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ps5_icc_raw",
	.fops = &icc_raw_fops,
};

static struct dentry *debug_spcie;
static struct dentry *debug_emc;

static int __init ps5_bar2_init(void)
{
	struct resource *res;

	/* spcie BAR2 (2MB) */
	spcie_pdev = pci_get_device(SONY_VID, SPCIE_DID, NULL);
	if (spcie_pdev) {
		res = &spcie_pdev->resource[2];
		if (res->start && resource_size(res) >= SPCIE_BAR2_SIZE) {
			if (request_mem_region(res->start, SPCIE_BAR2_SIZE, "ps5_bar2")) {
				spcie_bar2 = ioremap(res->start, SPCIE_BAR2_SIZE);
				if (spcie_bar2) {
					u8 first16[16];
					memcpy_fromio(first16, spcie_bar2, 16);
					pr_info("ps5_bar2: 2MB at %px: %*phN\n",
						spcie_bar2, 16, first16);
					debug_spcie = debugfs_create_file("ps5_bar2_dump", 0444, NULL, NULL, &spcie_bar2_fops);
					misc_register(&spcie_bar2_misc);
				}
			}
		}
	}

	/* tpcie BAR2 (512MB EMC DDR) — map first 16MB */
	tpcie_pdev = pci_get_device(SONY_VID, TPCIE_DID, NULL);
	if (tpcie_pdev) {
		res = &tpcie_pdev->resource[2];
		if (res->start && resource_size(res) >= EMC_DDR_DUMP_SIZE) {
			if (request_mem_region(res->start, EMC_DDR_DUMP_SIZE, "ps5_emc_ddr")) {
				emc_ddr_base = ioremap(res->start, EMC_DDR_DUMP_SIZE);
				if (emc_ddr_base) {
					pr_info("ps5_emc_ddr: 16MB at %px (phys %llx)\n",
						emc_ddr_base, (u64)res->start);
					debug_emc = debugfs_create_file("ps5_emc_ddr", 0444, NULL, NULL, &emc_ddr_fops);
					misc_register(&emc_ddr_misc);
				}
			}
		}
	}

	/* ICC raw query — uses existing icc_query() from spcie */
	misc_register(&icc_raw_misc);
	pr_info("ps5_dump: /dev/ps5_bar2, /dev/ps5_emc_ddr, /dev/ps5_icc_raw ready\n");
	return 0;
}

static void __exit ps5_bar2_exit(void)
{
	misc_deregister(&spcie_bar2_misc);
	misc_deregister(&emc_ddr_misc);
	misc_deregister(&icc_raw_misc);
	debugfs_remove(debug_spcie);
	debugfs_remove(debug_emc);

	if (spcie_bar2) {
		iounmap(spcie_bar2);
		if (spcie_pdev)
			release_mem_region(spcie_pdev->resource[2].start, SPCIE_BAR2_SIZE);
	}
	if (emc_ddr_base) {
		iounmap(emc_ddr_base);
		if (tpcie_pdev)
			release_mem_region(tpcie_pdev->resource[2].start, EMC_DDR_DUMP_SIZE);
	}
	pci_dev_put(spcie_pdev);
	pci_dev_put(tpcie_pdev);
	pr_info("ps5_dump: unloaded\n");
}

module_init(ps5_bar2_init);
module_exit(ps5_bar2_exit);

MODULE_AUTHOR("PS5 Linux");
MODULE_DESCRIPTION("PS5 BAR dump + ICC raw query module");
MODULE_LICENSE("GPL");
