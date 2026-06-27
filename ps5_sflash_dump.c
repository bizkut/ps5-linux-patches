// SPDX-License-Identifier: GPL-2.0
/* ps5_sflash_dump.c — Read sflash dump from reserved memory
 *
 * The ps5-linux-loader dumps the serial flash (NVS area) to a reserved
 * memory region after initrd in the cave area. The physical address and
 * size are passed via the linux_info struct, which is stored at a known
 * physical address in the cave area.
 *
 * This module reads the linux_info from the cave area to find the sflash
 * dump location, then exposes it via /proc/ps5_sflash_dump.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>

#define CAVE_BASE       0x100000000ULL
#define CAVE_LINUX_INFO 0x100005000ULL  /* cave_hv_code(0x3000) + 0x2000 */

/* Must match struct linux_info in ps5-linux-loader/include/linux.h */
struct tmr {
    u64 start;
    u64 end;
};

struct linux_info {
    uintptr_t linux_info;
    uintptr_t bzimage;
    size_t bzimage_size;
    uintptr_t initrd;
    size_t initrd_size;
    size_t vram_size;
    int kit_type;
    int n_tmrs;
    struct tmr tmrs[64];
    char cmdline[2048];
    uintptr_t sflash_dump;
    size_t sflash_size;
};

static phys_addr_t sflash_pa;
static size_t sflash_size;
static void __iomem *sflash_mem;
static struct proc_dir_entry *proc_entry;

static ssize_t sflash_proc_read(struct file *file, char __user *buf,
                                size_t count, loff_t *ppos)
{
    loff_t pos = *ppos;

    if (!sflash_mem || !sflash_size)
        return -ENODEV;

    if (pos >= sflash_size)
        return 0;
    if (pos + count > sflash_size)
        count = sflash_size - pos;

    if (copy_to_user(buf, sflash_mem + pos, count))
        return -EFAULT;

    *ppos = pos + count;
    return count;
}

static const struct proc_ops sflash_proc_ops = {
    .proc_read = sflash_proc_read,
    .proc_lseek = default_llseek,
};

static int __init ps5_sflash_dump_init(void)
{
    struct linux_info *info;
    void __iomem *info_mem;

    pr_info("ps5_sflash_dump: looking for linux_info at 0x%llx\n",
            (u64)CAVE_LINUX_INFO);

    info_mem = ioremap(CAVE_LINUX_INFO, sizeof(struct linux_info));
    if (!info_mem) {
        pr_err("ps5_sflash_dump: failed to map linux_info\n");
        return -ENOMEM;
    }

    info = (struct linux_info *)info_mem;
    sflash_pa = info->sflash_dump;
    sflash_size = info->sflash_size;

    pr_info("ps5_sflash_dump: sflash_dump=0x%llx size=0x%zx\n",
            (u64)sflash_pa, sflash_size);

    iounmap(info_mem);

    if (!sflash_pa || !sflash_size) {
        pr_err("ps5_sflash_dump: no sflash dump found\n");
        return -ENODEV;
    }

    sflash_mem = ioremap(sflash_pa, sflash_size);
    if (!sflash_mem) {
        pr_err("ps5_sflash_dump: failed to map sflash dump\n");
        return -ENOMEM;
    }

    proc_entry = proc_create("ps5_sflash_dump", 0444, NULL, &sflash_proc_ops);
    if (!proc_entry) {
        iounmap(sflash_mem);
        return -ENOMEM;
    }

    pr_info("ps5_sflash_dump: ready, %zu bytes at /proc/ps5_sflash_dump\n",
            sflash_size);
    return 0;
}

static void __exit ps5_sflash_dump_exit(void)
{
    if (proc_entry)
        proc_remove(proc_entry);
    if (sflash_mem)
        iounmap(sflash_mem);
    pr_info("ps5_sflash_dump: unloaded\n");
}

module_init(ps5_sflash_dump_init);
module_exit(ps5_sflash_dump_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PS5 serial flash dump reader from reserved memory");
