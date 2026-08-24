// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal /dev/ashmem for mainline 6.14. Android 16 HIDL FMQ and
 * SystemServer still create anonymous shared regions by opening this node.
 * Pin/purge is a no-op: those callers only need mmap-able named shmem.
 */
#define pr_fmt(fmt) "ashmem: " fmt

#include <linux/compat.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mutex.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <uapi/linux/ashmem.h>

#define ASHMEM_NAME_PREFIX	"dev/ashmem/"
#define ASHMEM_NAME_PREFIX_LEN	(sizeof(ASHMEM_NAME_PREFIX) - 1)
#define ASHMEM_FULL_NAME_LEN	(ASHMEM_NAME_LEN + ASHMEM_NAME_PREFIX_LEN)
#define PROT_MASK		(PROT_EXEC | PROT_READ | PROT_WRITE)

struct ashmem_area {
	char name[ASHMEM_FULL_NAME_LEN];
	struct file *file;
	size_t size;
	unsigned long prot_mask;
};

static DEFINE_MUTEX(ashmem_mutex);

static int ashmem_open(struct inode *inode, struct file *file)
{
	struct ashmem_area *asma;

	asma = kzalloc(sizeof(*asma), GFP_KERNEL);
	if (!asma)
		return -ENOMEM;
	memcpy(asma->name, ASHMEM_NAME_PREFIX, ASHMEM_NAME_PREFIX_LEN);
	asma->prot_mask = PROT_MASK;
	file->private_data = asma;
	return 0;
}

static int ashmem_release(struct inode *ignored, struct file *file)
{
	struct ashmem_area *asma = file->private_data;

	if (asma->file)
		fput(asma->file);
	kfree(asma);
	return 0;
}

static int ashmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ashmem_area *asma = file->private_data;
	int ret = 0;

	mutex_lock(&ashmem_mutex);
	if (!asma->size) {
		ret = -EINVAL;
		goto out;
	}
	if ((vma->vm_flags & VM_WRITE) && !(asma->prot_mask & PROT_WRITE)) {
		ret = -EPERM;
		goto out;
	}
	if ((vma->vm_flags & VM_EXEC) && !(asma->prot_mask & PROT_EXEC)) {
		ret = -EPERM;
		goto out;
	}
	if (!asma->file) {
		const char *name = ASHMEM_NAME_DEF;

		if (asma->name[ASHMEM_NAME_PREFIX_LEN] != '\0')
			name = asma->name;
		asma->file = shmem_file_setup(name, asma->size, vma->vm_flags);
		if (IS_ERR(asma->file)) {
			ret = PTR_ERR(asma->file);
			asma->file = NULL;
			goto out;
		}
	}
	vma_set_file(vma, asma->file);
	ret = call_mmap(asma->file, vma);
out:
	mutex_unlock(&ashmem_mutex);
	return ret;
}

static int set_name(struct ashmem_area *asma, void __user *name)
{
	int ret = 0;
	char local[ASHMEM_NAME_LEN];

	if (copy_from_user(local, name, sizeof(local)))
		return -EFAULT;
	local[ASHMEM_NAME_LEN - 1] = '\0';

	mutex_lock(&ashmem_mutex);
	if (asma->file) {
		ret = -EINVAL;
	} else {
		strcpy(asma->name + ASHMEM_NAME_PREFIX_LEN, local);
	}
	mutex_unlock(&ashmem_mutex);
	return ret;
}

static int get_name(struct ashmem_area *asma, void __user *name)
{
	int ret = 0;
	char local[ASHMEM_NAME_LEN];
	size_t len;

	mutex_lock(&ashmem_mutex);
	if (asma->name[ASHMEM_NAME_PREFIX_LEN] != '\0') {
		len = strlen(asma->name + ASHMEM_NAME_PREFIX_LEN) + 1;
		memcpy(local, asma->name + ASHMEM_NAME_PREFIX_LEN, len);
	} else {
		len = strlen(ASHMEM_NAME_DEF) + 1;
		memcpy(local, ASHMEM_NAME_DEF, len);
	}
	mutex_unlock(&ashmem_mutex);

	if (copy_to_user(name, local, len))
		ret = -EFAULT;
	return ret;
}

static long ashmem_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ashmem_area *asma = file->private_data;
	long ret = -ENOTTY;

	switch (cmd) {
	case ASHMEM_SET_NAME:
		ret = set_name(asma, (void __user *)arg);
		break;
	case ASHMEM_GET_NAME:
		ret = get_name(asma, (void __user *)arg);
		break;
	case ASHMEM_SET_SIZE:
		mutex_lock(&ashmem_mutex);
		if (asma->file) {
			ret = -EINVAL;
		} else {
			asma->size = (size_t)arg;
			ret = 0;
		}
		mutex_unlock(&ashmem_mutex);
		break;
	case ASHMEM_GET_SIZE:
		ret = (long)asma->size;
		break;
	case ASHMEM_SET_PROT_MASK:
		mutex_lock(&ashmem_mutex);
		if (arg & ~PROT_MASK)
			ret = -EINVAL;
		else if ((arg & asma->prot_mask) != arg)
			ret = -EINVAL;
		else {
			asma->prot_mask = arg;
			ret = 0;
		}
		mutex_unlock(&ashmem_mutex);
		break;
	case ASHMEM_GET_PROT_MASK:
		ret = (long)asma->prot_mask;
		break;
	case ASHMEM_PIN:
	case ASHMEM_UNPIN:
		ret = ASHMEM_NOT_PURGED;
		break;
	case ASHMEM_GET_PIN_STATUS:
		ret = ASHMEM_IS_PINNED;
		break;
	case ASHMEM_PURGE_ALL_CACHES:
		ret = 0;
		break;
	}
	return ret;
}

static const struct file_operations ashmem_fops = {
	.owner = THIS_MODULE,
	.open = ashmem_open,
	.release = ashmem_release,
	.mmap = ashmem_mmap,
	.unlocked_ioctl = ashmem_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static struct miscdevice ashmem_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ashmem",
	.fops = &ashmem_fops,
	.mode = 0666,
};

static int __init nabu_ashmem_init(void)
{
	int ret = misc_register(&ashmem_misc);

	if (ret) {
		pr_err("misc_register failed: %d\n", ret);
		return ret;
	}
	pr_info("initialized /dev/ashmem\n");
	return 0;
}
device_initcall(nabu_ashmem_init);
