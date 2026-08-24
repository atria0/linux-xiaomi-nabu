// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2014  Google, Inc.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include "internal.h"

static DEFINE_MUTEX(pmsg_lock);

static bool nabu_pmsg_mirror_enabled;

static int __init nabu_pmsg_mirror_setup(char *arg)
{
	nabu_pmsg_mirror_enabled = !arg || arg[0] != '0';
	return 0;
}
__setup("nabu_pmsg_mirror", nabu_pmsg_mirror_setup);

/*
 * liblog prefixes each write with a packed android_pmsg_log_header_t (7 bytes)
 * and android_log_header_t (11 bytes); the text after them is a priority byte
 * followed by a NUL separated tag and message.
 */
#define NABU_PMSG_HEADER_BYTES 18
#define NABU_PMSG_MIRROR_MAX 512

static void nabu_pmsg_mirror(const char __user *buf, size_t count)
{
	char line[NABU_PMSG_MIRROR_MAX];
	size_t copied;
	size_t i;

	if (!nabu_pmsg_mirror_enabled)
		return;
	if (count <= NABU_PMSG_HEADER_BYTES)
		return;

	copied = min(count, sizeof(line) - 1);
	if (copy_from_user(line, buf, copied))
		return;
	line[copied] = '\0';

	/*
	 * The headers and the tag/message separators are not printable, and a
	 * record can carry an embedded newline that would split the printk into
	 * an unattributed second line.
	 */
	for (i = 0; i < copied; i++) {
		if (line[i] < 0x20 || line[i] > 0x7e)
			line[i] = ' ';
	}

	pr_info("nabu-logcat:%s\n", line + NABU_PMSG_HEADER_BYTES);
}

static ssize_t write_pmsg(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct pstore_record record;
	int ret;

	if (!count)
		return 0;

	pstore_record_init(&record, psinfo);
	record.type = PSTORE_TYPE_PMSG;
	record.size = count;

	/* check outside lock, page in any data. write_user also checks */
	if (!access_ok(buf, count))
		return -EFAULT;

	nabu_pmsg_mirror(buf, count);

	mutex_lock(&pmsg_lock);
	ret = psinfo->write_user(&record, buf);
	mutex_unlock(&pmsg_lock);
	return ret ? ret : count;
}

static const struct file_operations pmsg_fops = {
	.owner		= THIS_MODULE,
	.llseek		= noop_llseek,
	.write		= write_pmsg,
};

static struct class *pmsg_class;
static int pmsg_major;
#define PMSG_NAME "pmsg"
#undef pr_fmt
#define pr_fmt(fmt) PMSG_NAME ": " fmt

static char *pmsg_devnode(const struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0220;
	return NULL;
}

void pstore_register_pmsg(void)
{
	struct device *pmsg_device;

	pmsg_major = register_chrdev(0, PMSG_NAME, &pmsg_fops);
	if (pmsg_major < 0) {
		pr_err("register_chrdev failed\n");
		goto err;
	}

	pmsg_class = class_create(PMSG_NAME);
	if (IS_ERR(pmsg_class)) {
		pr_err("device class file already in use\n");
		goto err_class;
	}
	pmsg_class->devnode = pmsg_devnode;

	pmsg_device = device_create(pmsg_class, NULL, MKDEV(pmsg_major, 0),
					NULL, "%s%d", PMSG_NAME, 0);
	if (IS_ERR(pmsg_device)) {
		pr_err("failed to create device\n");
		goto err_device;
	}
	return;

err_device:
	class_destroy(pmsg_class);
err_class:
	unregister_chrdev(pmsg_major, PMSG_NAME);
err:
	return;
}

void pstore_unregister_pmsg(void)
{
	device_destroy(pmsg_class, MKDEV(pmsg_major, 0));
	class_destroy(pmsg_class);
	unregister_chrdev(pmsg_major, PMSG_NAME);
}
