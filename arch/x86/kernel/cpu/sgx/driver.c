// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
// Copyright(c) 2016-18 Intel Corporation.

#include <linux/acpi.h>
#include <linux/miscdevice.h>
#include <linux/mman.h>
#include <linux/security.h>
#include <linux/suspend.h>
#include <asm/traps.h>
#include "driver.h"
#include "encl.h"

MODULE_DESCRIPTION("Intel SGX Enclave Driver");
MODULE_AUTHOR("Jarkko Sakkinen <jarkko.sakkinen@linux.intel.com>");
MODULE_LICENSE("Dual BSD/GPL");

u64 sgx_encl_size_max_32;
u64 sgx_encl_size_max_64;
u32 sgx_misc_reserved_mask;
u64 sgx_attributes_reserved_mask;
u64 sgx_xfrm_reserved_mask = ~0x3;
u32 sgx_xsave_size_tbl[64];

static int sgx_open(struct inode *inode, struct file *file)
{
	struct sgx_encl *encl;
	int ret;

	encl = kzalloc(sizeof(*encl), GFP_KERNEL);
	if (!encl)
		return -ENOMEM;

	atomic_set(&encl->flags, 0);
	kref_init(&encl->refcount);
	INIT_LIST_HEAD(&encl->va_pages);
	INIT_RADIX_TREE(&encl->page_tree, GFP_KERNEL);
	mutex_init(&encl->lock);
	INIT_LIST_HEAD(&encl->mm_list);
	spin_lock_init(&encl->mm_lock);

	ret = init_srcu_struct(&encl->srcu);
	if (ret) {
		kfree(encl);
		return ret;
	}

	file->private_data = encl;

	return 0;
}

static int sgx_release(struct inode *inode, struct file *file)
{
	struct sgx_encl *encl = file->private_data;
	struct sgx_encl_mm *encl_mm;

	for ( ; ; )  {
		spin_lock(&encl->mm_lock);

		if (list_empty(&encl->mm_list)) {
			encl_mm = NULL;
		} else {
			encl_mm = list_first_entry(&encl->mm_list,
						   struct sgx_encl_mm, list);
			list_del_rcu(&encl_mm->list);
		}

		spin_unlock(&encl->mm_lock);

		/* The list is empty, ready to go. */
		if (!encl_mm)
			break;

		synchronize_srcu(&encl->srcu);
		mmu_notifier_unregister(&encl_mm->mmu_notifier, encl_mm->mm);
		kfree(encl_mm);
	};

	mutex_lock(&encl->lock);
	atomic_or(SGX_ENCL_DEAD, &encl->flags);
	mutex_unlock(&encl->lock);

	kref_put(&encl->refcount, sgx_encl_release);
	return 0;
}

static int sgx_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sgx_encl *encl = file->private_data;
	int ret;

	ret = sgx_encl_may_map(encl, vma->vm_start, vma->vm_end,
			       vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC));
	if (ret)
		return ret;

	ret = sgx_encl_mm_add(encl, vma->vm_mm);
	if (ret)
		return ret;

	vma->vm_ops = &sgx_vm_ops;
	vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP | VM_IO;
	vma->vm_private_data = encl;

	return 0;
}

static unsigned long sgx_get_unmapped_area(struct file *file,
					   unsigned long addr,
					   unsigned long len,
					   unsigned long pgoff,
					   unsigned long flags)
{
	if (flags & MAP_PRIVATE)
		return -EINVAL;

	if (flags & MAP_FIXED)
		return addr;

	return current->mm->get_unmapped_area(file, addr, len, pgoff, flags);
}

#ifdef CONFIG_COMPAT
static long sgx_compat_ioctl(struct file *filep, unsigned int cmd,
			      unsigned long arg)
{
	return sgx_ioctl(filep, cmd, arg);
}
#endif

static const struct file_operations sgx_encl_fops = {
	.owner			= THIS_MODULE,
	.open			= sgx_open,
	.release		= sgx_release,
	.unlocked_ioctl		= sgx_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= sgx_compat_ioctl,
#endif
	.mmap			= sgx_mmap,
	.get_unmapped_area	= sgx_get_unmapped_area,
};

static struct miscdevice sgx_dev_enclave = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "enclave",
	.nodename = "sgx/enclave",
	.fops = &sgx_encl_fops,
};

int __init sgx_drv_init(void)
{
	unsigned int eax, ebx, ecx, edx;
	u64 attr_mask, xfrm_mask;
	int ret;
	int i;

	if (!boot_cpu_has(X86_FEATURE_SGX_LC)) {
		pr_info("The public key MSRs are not writable.\n");
		return -ENODEV;
	}

	cpuid_count(SGX_CPUID, 0, &eax, &ebx, &ecx, &edx);
	sgx_misc_reserved_mask = ~ebx | SGX_MISC_RESERVED_MASK;
	sgx_encl_size_max_64 = 1ULL << ((edx >> 8) & 0xFF);
	sgx_encl_size_max_32 = 1ULL << (edx & 0xFF);

	cpuid_count(SGX_CPUID, 1, &eax, &ebx, &ecx, &edx);

	attr_mask = (((u64)ebx) << 32) + (u64)eax;
	sgx_attributes_reserved_mask = ~attr_mask | SGX_ATTR_RESERVED_MASK;

	if (boot_cpu_has(X86_FEATURE_OSXSAVE)) {
		xfrm_mask = (((u64)edx) << 32) + (u64)ecx;

		for (i = 2; i < 64; i++) {
			cpuid_count(0x0D, i, &eax, &ebx, &ecx, &edx);
			if ((1 << i) & xfrm_mask)
				sgx_xsave_size_tbl[i] = eax + ebx;
		}

		sgx_xfrm_reserved_mask = ~xfrm_mask;
	}

	ret = misc_register(&sgx_dev_enclave);
	if (ret)
		pr_err("Creating /dev/sgx/enclave failed with %d.\n", ret);
	return ret;
}
