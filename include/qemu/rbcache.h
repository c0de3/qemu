/*
 * QEMU Range-Based Cache core
 *
 * Copyright (C) 2015-2016 Parallels IP Holdings GmbH. All rights reserved.
 *
 * Author: Pavel Butsykin <pbutsykin@virtuozzo.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#ifndef RBCACHE_H
#define RBCACHE_H

#include "qemu/rbtree.h"
#include "qemu/queue.h"

typedef struct RBCacheNode {
    struct RbNode rb_node;
    uint64_t offset;
    uint64_t bytes;
    QTAILQ_ENTRY(RBCacheNode) entry;
} RBCacheNode;

typedef struct RBCache RBCache;

typedef RBCacheNode *RBNodeAlloc(uint64_t offset, uint64_t bytes, void *opaque);
typedef void RBNodeFree(RBCacheNode *node, void *opaque);


enum eviction_type {
    RBCACHE_FIFO,
    RBCACHE_LRU,
};

/**
 * rbcache_search:
 * @rbcache: the cache object.
 * @offset: the start of the range.
 * @bytes: the size of the range.
 *
 * Returns the node corresponding to the range(offset, bytes),
 * or NULL if the node was not found.
 */
void *rbcache_search(RBCache *rbcache, uint64_t offset, uint64_t bytes);

/**
 * rbcache_insert:
 * @rbcache: the cache object.
 * @node: a new node for the cache.
 *
 * Returns the new node, or old node if the node already exists.
 */
void *rbcache_insert(RBCache *rbcache, RBCacheNode *node);

/**
 * rbcache_search_and_insert:
 * @rbcache: the cache object.
 * @offset: the start of the range.
 * @bytes: the size of the range.
 *
 * rbcache_search_and_insert() is like rbcache_insert(), except that a new node
 * is allocated inside the function. Returns the new node, or old node if the
 * node already exists.
 */
void *rbcache_search_and_insert(RBCache *rbcache, uint64_t offset,
                                uint64_t byte);

/**
 * rbcache_remove:
 * @rbcache: the cache object.
 * @node: the node to remove.
 *
 * Removes the cached range owned by the node.
 */
void rbcache_remove(RBCache *rbcache, RBCacheNode *node);

RBCacheNode *rbcache_node_alloc(RBCache *rbcache, uint64_t offset,
                                uint64_t bytes);

void rbcache_node_free(RBCache *rbcache, RBCacheNode *node);

/**
 * rbcache_create:
 * @alloc: callback to allocation node, allows to upgrade allocate and expand
 *         the capabilities of the node.
 * @free: callback to release node, must be used together with alloc callback.
 * @limit_size: maximum cache size in bytes.
 * @eviction_type: method of memory limitation
 * @opaque: the opaque pointer to pass to the callback.
 *
 * Returns the cache object.
 */
RBCache *rbcache_create(RBNodeAlloc *alloc, RBNodeFree *free,
                        uint64_t limit_size, int eviction_type, void *opaque);

/**
 * rbcache_destroy:
 * @rbcache: the cache object.
 *
 * Cleanup the cache object created with rbcache_create().
 */
void rbcache_destroy(RBCache *rbcache);

#endif /* RBCACHE_H */
