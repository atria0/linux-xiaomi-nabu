/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DMABUF Heaps Allocation Infrastructure
 *
 * Copyright (C) 2011 Google, Inc.
 * Copyright (C) 2019 Linaro Ltd.
 */

#ifndef _DMA_HEAPS_H
#define _DMA_HEAPS_H

#include <linux/types.h>

struct dma_buf;
struct dma_heap;

/**
 * struct dma_heap_ops - ops to operate on a given heap
 * @allocate:	allocate dmabuf and return struct dma_buf ptr
 *
 * allocate returns dmabuf on success, ERR_PTR(-errno) on error.
 */
struct dma_heap_ops {
	struct dma_buf *(*allocate)(struct dma_heap *heap,
				    unsigned long len,
				    u32 fd_flags,
				    u64 heap_flags);
};

/**
 * struct dma_heap_export_info - information needed to export a new dmabuf heap
 * @name:	used for debugging/device-node name
 * @ops:	ops struct for this heap
 * @priv:	heap exporter private data
 *
 * Information needed to export a new dmabuf heap.
 */
struct dma_heap_export_info {
	const char *name;
	const struct dma_heap_ops *ops;
	void *priv;
};

void *dma_heap_get_drvdata(struct dma_heap *heap);

const char *dma_heap_get_name(struct dma_heap *heap);

/**
 * dma_heap_find() - look up a permanently registered DMA heap by name
 * @name: heap name supplied at registration
 *
 * Return: Heap pointer, or NULL if no heap matches.
 */
struct dma_heap *dma_heap_find(const char *name);

/**
 * dma_heap_buffer_alloc() - allocate a DMA-BUF directly from a heap
 * @heap: heap returned by dma_heap_find() or dma_heap_add()
 * @len: allocation size in bytes
 * @fd_flags: file flags carried by the exported DMA-BUF
 * @heap_flags: heap-specific allocation flags
 *
 * Return: DMA-BUF with one reference, or ERR_PTR(-errno).
 */
struct dma_buf *dma_heap_buffer_alloc(struct dma_heap *heap, size_t len,
				      u32 fd_flags, u64 heap_flags);

struct dma_heap *dma_heap_add(const struct dma_heap_export_info *exp_info);

#endif /* _DMA_HEAPS_H */
