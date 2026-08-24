// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Qualcomm Secure Execution Environment (SEE) interface (QSEECOM).
 * Responsible for setting up and managing QSEECOM client devices.
 *
 * Copyright (C) 2023 Maximilian Luz <luzmaximilian@gmail.com>
 */
#include <linux/auxiliary_bus.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/elf.h>
#include <linux/fdtable.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/iosys-map.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <linux/firmware/qcom/qcom_qseecom.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>

#define QSEECOM_COMPAT_NAME		"qseecom"
#define ION_COMPAT_NAME		"ion"
#define QSEECOM_IOC_MAGIC		0x97
#define QSEECOM_MAX_APP_NAME_SIZE	64
#define QSEECOM_MAX_COMMAND_SIZE	SZ_1M
#define QSEECOM_MAX_IMAGE_SIZE		SZ_32M

/* 64-bit Android legacy ABI structures. */
struct qseecom_compat_register_listener_req {
	__u32 listener_id;
	__s32 ifd_data_fd;
	__u64 virt_sb_base;
	__u32 sb_size;
	__u32 pad;
};

struct qseecom_compat_send_cmd_req {
	__u64 cmd_req_buf;
	__u32 cmd_req_len;
	__u32 pad0;
	__u64 resp_buf;
	__u32 resp_len;
	__u32 pad1;
};

struct qseecom_compat_load_img_req {
	__u32 mdt_len;
	__u32 img_len;
	__s32 ifd_data_fd;
	char img_name[QSEECOM_MAX_APP_NAME_SIZE];
	__u32 app_arch;
	__u32 app_id;
};

struct qseecom_compat_set_mem_req {
	__s32 ifd_data_fd;
	__u32 pad0;
	__u64 virt_sb_base;
	__u32 sb_len;
	__u32 pad1;
};

struct qseecom_compat_app_query {
	char app_name[QSEECOM_MAX_APP_NAME_SIZE];
	__u32 app_id;
	__u32 app_arch;
};

static_assert(sizeof(struct qseecom_compat_register_listener_req) == 24);
static_assert(sizeof(struct qseecom_compat_send_cmd_req) == 32);
static_assert(sizeof(struct qseecom_compat_load_img_req) == 84);
static_assert(sizeof(struct qseecom_compat_set_mem_req) == 24);
static_assert(sizeof(struct qseecom_compat_app_query) == 72);

#define QSEECOM_IOCTL_REGISTER_LISTENER_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 1, struct qseecom_compat_register_listener_req)
#define QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ	_IO(QSEECOM_IOC_MAGIC, 2)
#define QSEECOM_IOCTL_SEND_CMD_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 3, struct qseecom_compat_send_cmd_req)
#define QSEECOM_IOCTL_SEND_MODFD_CMD_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 4, __u8[64])
#define QSEECOM_IOCTL_RECEIVE_REQ		_IO(QSEECOM_IOC_MAGIC, 5)
#define QSEECOM_IOCTL_SEND_RESP_REQ		_IO(QSEECOM_IOC_MAGIC, 6)
#define QSEECOM_IOCTL_LOAD_APP_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 7, struct qseecom_compat_load_img_req)
#define QSEECOM_IOCTL_SET_MEM_PARAM_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 8, struct qseecom_compat_set_mem_req)
#define QSEECOM_IOCTL_UNLOAD_APP_REQ	_IO(QSEECOM_IOC_MAGIC, 9)
#define QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 10, __u32)
#define QSEECOM_IOCTL_PERF_ENABLE_REQ	_IO(QSEECOM_IOC_MAGIC, 11)
#define QSEECOM_IOCTL_PERF_DISABLE_REQ	_IO(QSEECOM_IOC_MAGIC, 12)
#define QSEECOM_IOCTL_APP_LOADED_QUERY_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 15, struct qseecom_compat_app_query)
#define QSEECOM_IOCTL_SET_BUS_SCALING_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 23, __s32)
#define QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ \
	_IOWR(QSEECOM_IOC_MAGIC, 35, __u8[64])

static_assert(QSEECOM_IOCTL_REGISTER_LISTENER_REQ == 0xc0189701UL);
static_assert(QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ == 0x9702UL);
static_assert(QSEECOM_IOCTL_SEND_CMD_REQ == 0xc0209703UL);
static_assert(QSEECOM_IOCTL_RECEIVE_REQ == 0x9705UL);
static_assert(QSEECOM_IOCTL_SEND_RESP_REQ == 0x9706UL);
static_assert(QSEECOM_IOCTL_LOAD_APP_REQ == 0xc0549707UL);
static_assert(QSEECOM_IOCTL_SET_MEM_PARAM_REQ == 0xc0189708UL);
static_assert(QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ == 0xc004970aUL);
static_assert(QSEECOM_IOCTL_APP_LOADED_QUERY_REQ == 0xc048970fUL);

/* Android libion's non-legacy allocation ABI. */
struct ion_compat_allocation_data {
	__u64 len;
	__u32 heap_id_mask;
	__u32 flags;
	__u32 fd;
	__u32 unused;
};

static_assert(sizeof(struct ion_compat_allocation_data) == 24);

#define ION_IOC_MAGIC		'I'
#define ION_IOC_NEW_ALLOC \
	_IOWR(ION_IOC_MAGIC, 0, struct ion_compat_allocation_data)

static_assert(ION_IOC_NEW_ALLOC == 0xc0184900UL);

struct qseecom_app_desc {
	const char *app_name;
	const char *dev_name;
};

struct qseecom_compat_listener {
	struct kref refcount;
	struct list_head node;
	struct qseecom_compat *compat;
	struct dma_buf *dmabuf;
	void *tz_buffer;
	size_t size;
	u32 id;
	wait_queue_head_t request_wq;
	wait_queue_head_t response_wq;
	struct mutex state_lock;
	bool request_pending;
	bool response_pending;
	bool awaiting_response;
	bool abort;
};

struct qseecom_compat {
	struct device *dev;
	struct qcom_tzmem_pool *pool;
	struct miscdevice qsee_miscdev;
	struct miscdevice ion_miscdev;
	struct mutex commonlib_lock;
	struct mutex listener_lock;
	struct mutex app_lock;
	struct list_head listeners;
	bool commonlib_loaded;
	bool commonlib64_loaded;
};

struct qseecom_compat_file {
	struct qseecom_compat *compat;
	u32 app_id;
	u32 app_arch;
	u64 shared_buffer;
	u32 shared_buffer_len;
	char app_name[QSEECOM_MAX_APP_NAME_SIZE];
	struct qseecom_compat_listener *listener;
};

static int qseecom_firmware_header(const struct firmware *fw, u16 *segments,
				   u32 *arch)
{
	const unsigned char *ident;

	if (fw->size < EI_NIDENT)
		return -ENOEXEC;

	ident = fw->data;
	if (memcmp(ident, ELFMAG, SELFMAG))
		return -ENOEXEC;

	*arch = ident[EI_CLASS];
	switch (*arch) {
	case ELFCLASS32:
		if (fw->size < sizeof(Elf32_Ehdr))
			return -ENOEXEC;
		*segments = ((const Elf32_Ehdr *)fw->data)->e_phnum;
		break;
	case ELFCLASS64:
		if (fw->size < sizeof(Elf64_Ehdr))
			return -ENOEXEC;
		*segments = ((const Elf64_Ehdr *)fw->data)->e_phnum;
		break;
	default:
		return -ENOEXEC;
	}

	return *segments ? 0 : -ENOEXEC;
}

static int qseecom_firmware_name(char *buffer, size_t size, const char *name,
				 unsigned int segment)
{
	int ret;

	if (segment == UINT_MAX)
		ret = snprintf(buffer, size, "%s.mdt", name);
	else
		ret = snprintf(buffer, size, "%s.b%02u", name, segment);

	return ret < 0 || ret >= (int)size ? -ENAMETOOLONG : 0;
}

static int qseecom_collect_firmware(struct qseecom_compat *compat,
				    const char *name, void **image,
				    size_t *mdt_len, size_t *image_len,
				    u32 *arch)
{
	const struct firmware *fw = NULL;
	char fw_name[QSEECOM_MAX_APP_NAME_SIZE + 8];
	size_t total, offset;
	u16 segments;
	unsigned int i;
	void *buffer;
	int ret;

	ret = qseecom_firmware_name(fw_name, sizeof(fw_name), name, UINT_MAX);
	if (ret)
		return ret;

	ret = firmware_request_nowarn(&fw, fw_name, compat->dev);
	if (ret)
		return ret;

	ret = qseecom_firmware_header(fw, &segments, arch);
	if (ret)
		goto out_release;

	*mdt_len = fw->size;
	total = fw->size;
	release_firmware(fw);
	fw = NULL;

	for (i = 0; i < segments; i++) {
		ret = qseecom_firmware_name(fw_name, sizeof(fw_name), name, i);
		if (ret)
			return ret;
		ret = firmware_request_nowarn(&fw, fw_name, compat->dev);
		if (ret)
			return ret;
		if (check_add_overflow(total, fw->size, &total) ||
		    total > QSEECOM_MAX_IMAGE_SIZE) {
			ret = -EFBIG;
			goto out_release;
		}
		release_firmware(fw);
		fw = NULL;
	}

	buffer = qcom_tzmem_alloc(compat->pool, total, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;
	memset(buffer, 0, total);

	ret = qseecom_firmware_name(fw_name, sizeof(fw_name), name, UINT_MAX);
	if (ret)
		goto out_free;
	ret = firmware_request_nowarn(&fw, fw_name, compat->dev);
	if (ret)
		goto out_free;
	memcpy(buffer, fw->data, fw->size);
	offset = fw->size;
	release_firmware(fw);
	fw = NULL;

	for (i = 0; i < segments; i++) {
		ret = qseecom_firmware_name(fw_name, sizeof(fw_name), name, i);
		if (ret)
			goto out_free;
		ret = firmware_request_nowarn(&fw, fw_name, compat->dev);
		if (ret)
			goto out_free;
		memcpy(buffer + offset, fw->data, fw->size);
		offset += fw->size;
		release_firmware(fw);
		fw = NULL;
	}

	*image = buffer;
	*image_len = total;
	return 0;

out_release:
	release_firmware(fw);
	return ret;
out_free:
	release_firmware(fw);
	qcom_tzmem_free(buffer);
	return ret;
}

static int qseecom_ensure_commonlib(struct qseecom_compat *compat, u32 app_arch)
{
	const char *name;
	bool *loaded;
	size_t mdt_len, image_len;
	u32 image_arch;
	void *image;
	int ret;

	if (app_arch == ELFCLASS64) {
		name = "cmnlib64";
		loaded = &compat->commonlib64_loaded;
	} else if (app_arch == ELFCLASS32) {
		name = "cmnlib";
		loaded = &compat->commonlib_loaded;
	} else {
		return -EINVAL;
	}

	mutex_lock(&compat->commonlib_lock);
	if (*loaded) {
		ret = 0;
		goto out_unlock;
	}

	ret = qseecom_collect_firmware(compat, name, &image, &mdt_len,
					 &image_len, &image_arch);
	if (ret)
		goto out_unlock;
	if (image_arch != app_arch) {
		ret = -ENOEXEC;
		goto out_free;
	}

	ret = qcom_scm_qseecom_load_service_image(image, mdt_len, image_len);
	if (!ret) {
		*loaded = true;
		dev_info(compat->dev, "legacy compat loaded %s (%zu bytes)\n",
			 name, image_len);
	}

out_free:
	qcom_tzmem_free(image);
out_unlock:
	mutex_unlock(&compat->commonlib_lock);
	return ret;
}

static int qseecom_copy_dmabuf(int fd, void *destination, size_t len)
{
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	struct dma_buf *dmabuf;
	int ret;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	if (len > dmabuf->size) {
		ret = -EINVAL;
		goto out_put;
	}

	ret = dma_buf_begin_cpu_access(dmabuf, DMA_TO_DEVICE);
	if (ret)
		goto out_put;
	ret = dma_buf_vmap(dmabuf, &map);
	if (ret)
		goto out_end;
	iosys_map_memcpy_from(destination, &map, 0, len);
	dma_buf_vunmap(dmabuf, &map);

out_end:
	dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);
out_put:
	dma_buf_put(dmabuf);
	return ret;
}

static int qseecom_dmabuf_to_tzmem(struct qseecom_compat_listener *listener)
{
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	int ret;

	ret = dma_buf_begin_cpu_access(listener->dmabuf, DMA_BIDIRECTIONAL);
	if (ret)
		return ret;
	ret = dma_buf_vmap(listener->dmabuf, &map);
	if (!ret) {
		iosys_map_memcpy_from(listener->tz_buffer, &map, 0,
				      listener->size);
		dma_buf_vunmap(listener->dmabuf, &map);
	}
	dma_buf_end_cpu_access(listener->dmabuf, DMA_BIDIRECTIONAL);

	return ret;
}

static int qseecom_tzmem_to_dmabuf(struct qseecom_compat_listener *listener)
{
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(NULL);
	int ret;

	ret = dma_buf_begin_cpu_access(listener->dmabuf, DMA_BIDIRECTIONAL);
	if (ret)
		return ret;
	ret = dma_buf_vmap(listener->dmabuf, &map);
	if (!ret) {
		iosys_map_memcpy_to(&map, 0, listener->tz_buffer,
				    listener->size);
		dma_buf_vunmap(listener->dmabuf, &map);
	}
	dma_buf_end_cpu_access(listener->dmabuf, DMA_BIDIRECTIONAL);

	return ret;
}

static void qseecom_listener_release(struct kref *refcount)
{
	struct qseecom_compat_listener *listener;

	listener = container_of(refcount, struct qseecom_compat_listener,
				refcount);
	if (listener->tz_buffer)
		qcom_tzmem_free(listener->tz_buffer);
	if (listener->dmabuf)
		dma_buf_put(listener->dmabuf);
	kfree(listener);
}

static void qseecom_listener_put(struct qseecom_compat_listener *listener)
{
	kref_put(&listener->refcount, qseecom_listener_release);
}

static struct qseecom_compat_listener *
qseecom_listener_get_for_file(struct qseecom_compat_file *ctx)
{
	struct qseecom_compat_listener *listener;

	mutex_lock(&ctx->compat->listener_lock);
	listener = ctx->listener;
	if (listener)
		kref_get(&listener->refcount);
	mutex_unlock(&ctx->compat->listener_lock);

	return listener;
}

static struct qseecom_compat_listener *
qseecom_listener_get_by_id(struct qseecom_compat *compat, u32 listener_id)
{
	struct qseecom_compat_listener *listener;

	mutex_lock(&compat->listener_lock);
	list_for_each_entry(listener, &compat->listeners, node) {
		if (listener->id == listener_id && !listener->abort) {
			kref_get(&listener->refcount);
			mutex_unlock(&compat->listener_lock);
			return listener;
		}
	}
	mutex_unlock(&compat->listener_lock);

	return NULL;
}

static long qseecom_ioctl_load_app(struct qseecom_compat_file *ctx,
				   void __user *argp)
{
	struct qseecom_compat_load_img_req request;
	void *image;
	u32 app_id;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	request.img_name[QSEECOM_MAX_APP_NAME_SIZE - 1] = '\0';
	if (!request.img_name[0] || !request.mdt_len ||
	    request.mdt_len > request.img_len ||
	    request.img_len > QSEECOM_MAX_IMAGE_SIZE)
		return -EINVAL;

	ret = qcom_scm_qseecom_app_get_id(request.img_name, &app_id);
	if (ret == -ENOENT) {
		ret = qseecom_ensure_commonlib(ctx->compat, request.app_arch);
		if (ret)
			return ret;

		image = qcom_tzmem_alloc(ctx->compat->pool, request.img_len,
					    GFP_KERNEL);
		if (!image)
			return -ENOMEM;
		ret = qseecom_copy_dmabuf(request.ifd_data_fd, image,
					  request.img_len);
		if (!ret)
			ret = qcom_scm_qseecom_app_start(image, request.mdt_len,
						 request.img_len, &app_id);
		qcom_tzmem_free(image);
	}
	if (ret)
		return ret;

	ctx->app_id = app_id;
	ctx->app_arch = request.app_arch;
	strscpy(ctx->app_name, request.img_name, sizeof(ctx->app_name));
	request.app_id = app_id;
	if (copy_to_user(argp, &request, sizeof(request)))
		return -EFAULT;

	dev_info(ctx->compat->dev, "legacy compat selected app %s id=%u\n",
		 ctx->app_name, ctx->app_id);
	return 0;
}

static long qseecom_ioctl_query_app(struct qseecom_compat_file *ctx,
				    void __user *argp)
{
	struct qseecom_compat_app_query query;
	u32 app_id;
	int ret;

	if (copy_from_user(&query, argp, sizeof(query)))
		return -EFAULT;
	query.app_name[QSEECOM_MAX_APP_NAME_SIZE - 1] = '\0';
	if (!query.app_name[0])
		return -EINVAL;

	ret = qcom_scm_qseecom_app_get_id(query.app_name, &app_id);
	if (ret == -ENOENT) {
		query.app_id = 0;
		ret = 0;
	} else if (!ret) {
		query.app_id = app_id;
		ctx->app_id = app_id;
		ctx->app_arch = query.app_arch;
		strscpy(ctx->app_name, query.app_name, sizeof(ctx->app_name));
	}
	if (ret)
		return ret;
	if (copy_to_user(argp, &query, sizeof(query)))
		return -EFAULT;

	/* Match downstream QSEECom: EEXIST means the app is already loaded. */
	if (query.app_id)
		return -EEXIST;
	return 0;
}

static long qseecom_ioctl_set_mem(struct qseecom_compat_file *ctx,
				  void __user *argp)
{
	struct qseecom_compat_set_mem_req request;
	struct dma_buf *dmabuf;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	if (!request.virt_sb_base || !request.sb_len)
		return -EINVAL;

	dmabuf = dma_buf_get(request.ifd_data_fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	if (request.sb_len > dmabuf->size) {
		dma_buf_put(dmabuf);
		return -EINVAL;
	}
	dma_buf_put(dmabuf);

	ctx->shared_buffer = request.virt_sb_base;
	ctx->shared_buffer_len = request.sb_len;
	return 0;
}

static long qseecom_ioctl_register_listener(struct qseecom_compat_file *ctx,
					     void __user *argp)
{
	struct qseecom_compat_register_listener_req request;
	struct qseecom_compat_listener *listener, *other;
	struct qseecom_compat *compat = ctx->compat;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	if (!request.listener_id || !request.virt_sb_base || !request.sb_size ||
	    request.sb_size > QSEECOM_MAX_COMMAND_SIZE)
		return -EINVAL;

	listener = kzalloc(sizeof(*listener), GFP_KERNEL);
	if (!listener)
		return -ENOMEM;
	kref_init(&listener->refcount);
	INIT_LIST_HEAD(&listener->node);
	listener->compat = compat;
	listener->id = request.listener_id;
	listener->size = request.sb_size;
	init_waitqueue_head(&listener->request_wq);
	init_waitqueue_head(&listener->response_wq);
	mutex_init(&listener->state_lock);

	listener->dmabuf = dma_buf_get(request.ifd_data_fd);
	if (IS_ERR(listener->dmabuf)) {
		ret = PTR_ERR(listener->dmabuf);
		listener->dmabuf = NULL;
		goto out_free_listener;
	}
	if (listener->size > listener->dmabuf->size) {
		ret = -EINVAL;
		goto out_free_listener;
	}
	listener->tz_buffer = qcom_tzmem_alloc(compat->pool, listener->size,
						   GFP_KERNEL);
	if (!listener->tz_buffer) {
		ret = -ENOMEM;
		goto out_free_listener;
	}

	ret = qseecom_dmabuf_to_tzmem(listener);
	if (ret)
		goto out_free_listener;

	/* Serialize registration with SEND_CMD callback processing. */
	mutex_lock(&compat->app_lock);
	mutex_lock(&compat->listener_lock);
	if (ctx->listener) {
		ret = -EBUSY;
		goto out_unlock_listener;
	}
	list_for_each_entry(other, &compat->listeners, node) {
		if (other->id == listener->id) {
			ret = -EBUSY;
			goto out_unlock_listener;
		}
	}
	mutex_unlock(&compat->listener_lock);

	ret = qcom_scm_qseecom_register_listener(listener->id,
						listener->tz_buffer,
						listener->size);
	if (ret)
		goto out_unlock_app;

	mutex_lock(&compat->listener_lock);
	ctx->listener = listener;
	list_add_tail(&listener->node, &compat->listeners);
	mutex_unlock(&compat->listener_lock);
	mutex_unlock(&compat->app_lock);

	dev_info(compat->dev,
		 "legacy listener registered id=%u size=%zu\n",
		 listener->id, listener->size);
	return 0;

out_unlock_listener:
	mutex_unlock(&compat->listener_lock);
out_unlock_app:
	mutex_unlock(&compat->app_lock);
out_free_listener:
	qseecom_listener_put(listener);
	return ret;
}

static long qseecom_ioctl_unregister_listener(struct qseecom_compat_file *ctx)
{
	struct qseecom_compat_listener *listener;
	struct qseecom_compat *compat = ctx->compat;
	int ret;

	mutex_lock(&compat->listener_lock);
	listener = ctx->listener;
	if (!listener) {
		mutex_unlock(&compat->listener_lock);
		return -ENODATA;
	}
	ctx->listener = NULL;
	list_del_init(&listener->node);
	mutex_lock(&listener->state_lock);
	listener->abort = true;
	listener->request_pending = false;
	listener->response_pending = false;
	listener->awaiting_response = false;
	mutex_unlock(&listener->state_lock);
	mutex_unlock(&compat->listener_lock);
	wake_up_interruptible(&listener->request_wq);
	wake_up_interruptible(&listener->response_wq);

	/* A current app transaction drops app_lock after leaving the callback. */
	mutex_lock(&compat->app_lock);
	ret = qcom_scm_qseecom_unregister_listener(listener->id);
	mutex_unlock(&compat->app_lock);

	dev_info(compat->dev,
		 "legacy listener unregistered id=%u ret=%d\n",
		 listener->id, ret);
	qseecom_listener_put(listener);
	return ret;
}

static long qseecom_ioctl_receive_req(struct qseecom_compat_file *ctx)
{
	struct qseecom_compat_listener *listener;
	int ret;

	listener = qseecom_listener_get_for_file(ctx);
	if (!listener)
		return -ENODATA;

	ret = wait_event_interruptible(listener->request_wq,
			READ_ONCE(listener->request_pending) ||
			READ_ONCE(listener->abort));
	if (ret)
		goto out;

	mutex_lock(&listener->state_lock);
	if (listener->abort)
		ret = -ENODEV;
	else
		listener->request_pending = false;
	mutex_unlock(&listener->state_lock);

out:
	qseecom_listener_put(listener);
	return ret;
}

static long qseecom_ioctl_send_resp(struct qseecom_compat_file *ctx)
{
	struct qseecom_compat_listener *listener;
	int ret;

	listener = qseecom_listener_get_for_file(ctx);
	if (!listener)
		return -ENODATA;

	mutex_lock(&listener->state_lock);
	if (listener->abort) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!listener->awaiting_response) {
		ret = -EINVAL;
		goto out_unlock;
	}
	mutex_unlock(&listener->state_lock);

	ret = qseecom_dmabuf_to_tzmem(listener);
	if (ret)
		goto out;

	mutex_lock(&listener->state_lock);
	if (listener->abort || !listener->awaiting_response) {
		ret = -ENODEV;
		goto out_unlock;
	}
	listener->response_pending = true;
	mutex_unlock(&listener->state_lock);
	wake_up_interruptible(&listener->response_wq);
	ret = 0;
	goto out;

out_unlock:
	mutex_unlock(&listener->state_lock);
out:
	qseecom_listener_put(listener);
	return ret;
}

static int qseecom_process_incomplete(struct qseecom_compat_file *ctx,
				      struct qcom_scm_qseecom_response *response)
{
	struct qseecom_compat_listener *listener;
	u32 listener_id, response_status;
	int ret, scm_ret;

	while (response->result == QCOM_QSEECOM_RESULT_INCOMPLETE) {
		if (response->data > U32_MAX)
			return -ERANGE;
		listener_id = response->data;
		listener = qseecom_listener_get_by_id(ctx->compat, listener_id);
		response_status = QCOM_QSEECOM_RESULT_FAILURE;
		ret = -ENODEV;

		if (!listener) {
			dev_err(ctx->compat->dev,
				"QSEE callback requested missing listener id=%u\n",
				listener_id);
			goto send_response;
		}

		ret = qseecom_tzmem_to_dmabuf(listener);
		if (ret)
			goto send_response;

		mutex_lock(&listener->state_lock);
		if (listener->abort) {
			ret = -ENODEV;
			mutex_unlock(&listener->state_lock);
			goto send_response;
		}
		listener->response_pending = false;
		listener->awaiting_response = true;
		listener->request_pending = true;
		mutex_unlock(&listener->state_lock);
		wake_up_interruptible(&listener->request_wq);

		ret = wait_event_interruptible(listener->response_wq,
				READ_ONCE(listener->response_pending) ||
				READ_ONCE(listener->abort));

		mutex_lock(&listener->state_lock);
		if (!ret && listener->abort)
			ret = -ENODEV;
		if (!ret && listener->response_pending)
			response_status = QCOM_QSEECOM_RESULT_SUCCESS;
		listener->request_pending = false;
		listener->response_pending = false;
		listener->awaiting_response = false;
		mutex_unlock(&listener->state_lock);

send_response:
		scm_ret = qcom_scm_qseecom_listener_response(listener_id,
							 response_status,
							 response);
		if (listener)
			qseecom_listener_put(listener);
		if (scm_ret)
			return scm_ret;
		if (ret)
			return ret;
		if (response->result ==
		    QCOM_QSEECOM_RESULT_BLOCKED_ON_LISTENER) {
			dev_err(ctx->compat->dev,
				"QSEE re-entrant blocked listener is unsupported\n");
			return -EOPNOTSUPP;
		}
		if (response->result != QCOM_QSEECOM_RESULT_SUCCESS &&
		    response->result != QCOM_QSEECOM_RESULT_INCOMPLETE)
			return -EIO;
	}

	return 0;
}

static long qseecom_ioctl_send_cmd(struct qseecom_compat_file *ctx,
				   void __user *argp)
{
	struct qseecom_compat_send_cmd_req request;
	struct qcom_scm_qseecom_response response = {};
	void *req = NULL, *rsp = NULL;
	int ret;

	if (!ctx->app_id)
		return -ENODEV;
	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	if (!request.cmd_req_buf || !request.resp_buf ||
	    !request.cmd_req_len || !request.resp_len ||
	    request.cmd_req_len > QSEECOM_MAX_COMMAND_SIZE ||
	    request.resp_len > QSEECOM_MAX_COMMAND_SIZE)
		return -EINVAL;

	req = qcom_tzmem_alloc(ctx->compat->pool, request.cmd_req_len,
				GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	rsp = qcom_tzmem_alloc(ctx->compat->pool, request.resp_len,
				GFP_KERNEL);
	if (!rsp) {
		ret = -ENOMEM;
		goto out;
	}
	memset(rsp, 0, request.resp_len);
	if (copy_from_user(req, u64_to_user_ptr(request.cmd_req_buf),
			   request.cmd_req_len)) {
		ret = -EFAULT;
		goto out;
	}

	mutex_lock(&ctx->compat->app_lock);
	ret = qcom_scm_qseecom_app_send_raw(ctx->app_id, req,
					     request.cmd_req_len, rsp,
					     request.resp_len, &response);
	if (!ret && response.result == QCOM_QSEECOM_RESULT_INCOMPLETE)
		ret = qseecom_process_incomplete(ctx, &response);
	if (!ret && response.result ==
	    QCOM_QSEECOM_RESULT_BLOCKED_ON_LISTENER)
		ret = -EOPNOTSUPP;
	if (!ret && response.result != QCOM_QSEECOM_RESULT_SUCCESS)
		ret = -EIO;
	mutex_unlock(&ctx->compat->app_lock);
	if (!ret && copy_to_user(u64_to_user_ptr(request.resp_buf), rsp,
				 request.resp_len))
		ret = -EFAULT;

out:
	qcom_tzmem_free(rsp);
	qcom_tzmem_free(req);
	return ret;
}

static const char *qseecom_compat_ioctl_name(unsigned int command)
{
	switch (command) {
	case QSEECOM_IOCTL_REGISTER_LISTENER_REQ:
		return "REGISTER_LISTENER";
	case QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ:
		return "UNREGISTER_LISTENER";
	case QSEECOM_IOCTL_SEND_CMD_REQ:
		return "SEND_CMD";
	case QSEECOM_IOCTL_SEND_MODFD_CMD_REQ:
		return "SEND_MODFD_CMD";
	case QSEECOM_IOCTL_RECEIVE_REQ:
		return "RECEIVE";
	case QSEECOM_IOCTL_SEND_RESP_REQ:
		return "SEND_RESP";
	case QSEECOM_IOCTL_LOAD_APP_REQ:
		return "LOAD_APP";
	case QSEECOM_IOCTL_SET_MEM_PARAM_REQ:
		return "SET_MEM_PARAM";
	case QSEECOM_IOCTL_APP_LOADED_QUERY_REQ:
		return "APP_LOADED_QUERY";
	default:
		return "other";
	}
}

static long qseecom_compat_ioctl(struct file *file, unsigned int command,
				 unsigned long argument)
{
	struct qseecom_compat_file *ctx = file->private_data;
	void __user *argp = (void __user *)argument;
	u32 version;
	int ret;

	dev_info_ratelimited(ctx->compat->dev,
		"legacy qseecom ioctl=%#x nr=%u name=%s pid=%d comm=%s\n",
		command, _IOC_NR(command), qseecom_compat_ioctl_name(command),
		task_pid_nr(current), current->comm);

	switch (command) {
	case QSEECOM_IOCTL_GET_QSEOS_VERSION_REQ:
		ret = qcom_scm_qseecom_get_version(&version);
		if (ret)
			return ret;
		return copy_to_user(argp, &version, sizeof(version)) ? -EFAULT : 0;
	case QSEECOM_IOCTL_APP_LOADED_QUERY_REQ:
		return qseecom_ioctl_query_app(ctx, argp);
	case QSEECOM_IOCTL_SET_MEM_PARAM_REQ:
		return qseecom_ioctl_set_mem(ctx, argp);
	case QSEECOM_IOCTL_LOAD_APP_REQ:
		return qseecom_ioctl_load_app(ctx, argp);
	case QSEECOM_IOCTL_SEND_CMD_REQ:
		return qseecom_ioctl_send_cmd(ctx, argp);
	case QSEECOM_IOCTL_REGISTER_LISTENER_REQ:
		return qseecom_ioctl_register_listener(ctx, argp);
	case QSEECOM_IOCTL_UNREGISTER_LISTENER_REQ:
		return qseecom_ioctl_unregister_listener(ctx);
	case QSEECOM_IOCTL_RECEIVE_REQ:
		return qseecom_ioctl_receive_req(ctx);
	case QSEECOM_IOCTL_SEND_RESP_REQ:
		return qseecom_ioctl_send_resp(ctx);
	case QSEECOM_IOCTL_UNLOAD_APP_REQ:
		ctx->app_id = 0;
		ctx->app_name[0] = '\0';
		return 0;
	case QSEECOM_IOCTL_PERF_ENABLE_REQ:
	case QSEECOM_IOCTL_PERF_DISABLE_REQ:
	case QSEECOM_IOCTL_SET_BUS_SCALING_REQ:
		return 0;
	case QSEECOM_IOCTL_SEND_MODFD_CMD_REQ:
	case QSEECOM_IOCTL_SEND_MODFD_CMD_64_REQ:
		dev_warn_ratelimited(ctx->compat->dev,
			"legacy qseecom listener/modfd ioctl not implemented: %#x (%s)\n",
			command, qseecom_compat_ioctl_name(command));
		return -EOPNOTSUPP;
	default:
		dev_warn_ratelimited(ctx->compat->dev,
			"legacy qseecom unsupported ioctl %#x\n", command);
		return -ENOTTY;
	}
}

static int qseecom_compat_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct qseecom_compat *compat;
	struct qseecom_compat_file *ctx;

	compat = container_of(misc, struct qseecom_compat, qsee_miscdev);
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->compat = compat;
	file->private_data = ctx;
	return nonseekable_open(inode, file);
}

static int qseecom_compat_release(struct inode *inode, struct file *file)
{
	struct qseecom_compat_file *ctx = file->private_data;

	qseecom_ioctl_unregister_listener(ctx);
	kfree(ctx);
	return 0;
}

static const struct file_operations qseecom_compat_fops = {
	.owner = THIS_MODULE,
	.open = qseecom_compat_open,
	.release = qseecom_compat_release,
	.unlocked_ioctl = qseecom_compat_ioctl,
	.compat_ioctl = qseecom_compat_ioctl,
	.llseek = noop_llseek,
};

static long ion_compat_ioctl(struct file *file, unsigned int command,
			     unsigned long argument)
{
	struct ion_compat_allocation_data allocation;
	struct dma_heap *heap;
	struct dma_buf *dmabuf;
	void __user *argp = (void __user *)argument;
	int fd;

	/* ENOTTY for ABI_VERSION makes Android libion use ION_IOC_NEW_ALLOC. */
	if (command != ION_IOC_NEW_ALLOC)
		return -ENOTTY;
	if (copy_from_user(&allocation, argp, sizeof(allocation)))
		return -EFAULT;
	if (!allocation.len || allocation.len > QSEECOM_MAX_IMAGE_SIZE)
		return -EINVAL;

	heap = dma_heap_find("system");
	if (!heap)
		return -EPROBE_DEFER;
	dmabuf = dma_heap_buffer_alloc(heap, allocation.len,
				       O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	fd = dma_buf_fd(dmabuf, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(dmabuf);
		return fd;
	}
	allocation.fd = fd;
	if (copy_to_user(argp, &allocation, sizeof(allocation))) {
		close_fd(fd);
		return -EFAULT;
	}

	return 0;
}

static const struct file_operations ion_compat_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ion_compat_ioctl,
	.compat_ioctl = ion_compat_ioctl,
	.llseek = noop_llseek,
};

static void qseecom_misc_deregister(void *data)
{
	misc_deregister(data);
}

static void qseecom_client_release(struct device *dev)
{
	struct qseecom_client *client;

	client = container_of(dev, struct qseecom_client, aux_dev.dev);
	kfree(client);
}

static void qseecom_client_remove(void *data)
{
	struct qseecom_client *client = data;

	auxiliary_device_delete(&client->aux_dev);
	auxiliary_device_uninit(&client->aux_dev);
}

static int qseecom_client_register(struct platform_device *qseecom_dev,
				   const struct qseecom_app_desc *desc)
{
	struct qseecom_client *client;
	u32 app_id;
	int ret;

	/* Try to find the app ID, skip device if not found */
	ret = qcom_scm_qseecom_app_get_id(desc->app_name, &app_id);
	if (ret)
		return ret == -ENOENT ? 0 : ret;

	dev_info(&qseecom_dev->dev, "setting up client for %s\n", desc->app_name);

	/* Allocate and set-up the client device */
	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	client->aux_dev.name = desc->dev_name;
	client->aux_dev.dev.parent = &qseecom_dev->dev;
	client->aux_dev.dev.release = qseecom_client_release;
	client->app_id = app_id;

	ret = auxiliary_device_init(&client->aux_dev);
	if (ret) {
		kfree(client);
		return ret;
	}

	ret = auxiliary_device_add(&client->aux_dev);
	if (ret) {
		auxiliary_device_uninit(&client->aux_dev);
		return ret;
	}

	ret = devm_add_action_or_reset(&qseecom_dev->dev, qseecom_client_remove,
				       client);
	if (ret)
		return ret;

	return 0;
}

/*
 * List of supported applications. One client device will be created per entry,
 * assuming the app has already been loaded (usually by firmware bootloaders)
 * and its ID can be queried successfully.
 */
static const struct qseecom_app_desc qcom_qseecom_apps[] = {
	{ "qcom.tz.uefisecapp", "uefisecapp" },
};

static int qcom_qseecom_probe(struct platform_device *qseecom_dev)
{
	static const struct qcom_tzmem_pool_config pool_config = {
		.initial_size = SZ_256K,
		.policy = QCOM_TZMEM_POLICY_ON_DEMAND,
		.max_size = SZ_64M,
	};
	struct qseecom_compat *compat;
	int ret;
	int i;

	compat = devm_kzalloc(&qseecom_dev->dev, sizeof(*compat), GFP_KERNEL);
	if (!compat)
		return -ENOMEM;
	compat->dev = &qseecom_dev->dev;
	mutex_init(&compat->commonlib_lock);
	mutex_init(&compat->listener_lock);
	mutex_init(&compat->app_lock);
	INIT_LIST_HEAD(&compat->listeners);
	compat->pool = devm_qcom_tzmem_pool_new(&qseecom_dev->dev, &pool_config);
	if (IS_ERR(compat->pool))
		return dev_err_probe(&qseecom_dev->dev, PTR_ERR(compat->pool),
				     "failed to create legacy TZMEM pool\n");

	compat->qsee_miscdev.minor = MISC_DYNAMIC_MINOR;
	compat->qsee_miscdev.name = QSEECOM_COMPAT_NAME;
	compat->qsee_miscdev.fops = &qseecom_compat_fops;
	compat->qsee_miscdev.parent = &qseecom_dev->dev;
	compat->qsee_miscdev.mode = 0660;
	ret = misc_register(&compat->qsee_miscdev);
	if (ret)
		return dev_err_probe(&qseecom_dev->dev, ret,
				     "failed to register /dev/qseecom\n");
	ret = devm_add_action_or_reset(&qseecom_dev->dev,
				       qseecom_misc_deregister,
				       &compat->qsee_miscdev);
	if (ret)
		return ret;

	compat->ion_miscdev.minor = MISC_DYNAMIC_MINOR;
	compat->ion_miscdev.name = ION_COMPAT_NAME;
	compat->ion_miscdev.fops = &ion_compat_fops;
	compat->ion_miscdev.parent = &qseecom_dev->dev;
	compat->ion_miscdev.mode = 0664;
	ret = misc_register(&compat->ion_miscdev);
	if (ret)
		return dev_err_probe(&qseecom_dev->dev, ret,
				     "failed to register /dev/ion\n");
	ret = devm_add_action_or_reset(&qseecom_dev->dev,
				       qseecom_misc_deregister,
				       &compat->ion_miscdev);
	if (ret)
		return ret;

	platform_set_drvdata(qseecom_dev, compat);
	dev_info(&qseecom_dev->dev,
		 "registered Android legacy /dev/qseecom and /dev/ion bridges\n");

	/* Set up client devices for each base application */
	for (i = 0; i < ARRAY_SIZE(qcom_qseecom_apps); i++) {
		ret = qseecom_client_register(qseecom_dev, &qcom_qseecom_apps[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static struct platform_driver qcom_qseecom_driver = {
	.driver = {
		.name = "qcom_qseecom",
	},
	.probe = qcom_qseecom_probe,
};

static int __init qcom_qseecom_init(void)
{
	return platform_driver_register(&qcom_qseecom_driver);
}
subsys_initcall(qcom_qseecom_init);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Driver for the Qualcomm SEE (QSEECOM) interface");
MODULE_LICENSE("GPL");
