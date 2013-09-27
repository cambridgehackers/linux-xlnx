/*
 * Generic bridge to memory-mapped hardware
 *
 * Author: Jamey Hicks <jamey.hicks@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#define DEBUG
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/uaccess.h>
#include <linux/console.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include "portal.h"
#include <linux/portal.h>
#include <linux/clk.h>
#include <linux/seq_file.h>
#include <linux/memblock.h>

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/dma-buf.h>
#include <linux/portal.h>

#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <asm/cacheflush.h>


/////////////////////////////////////////////////////////////
// copied from ion.c

/**
 * struct ion_buffer - metadata for a particular buffer
 * @ref:		refernce count
 * @node:		node in the ion_device buffers tree
 * @dev:		back pointer to the ion_device
 * @heap:		back pointer to the heap the buffer came from
 * @flags:		buffer specific flags
 * @size:		size of the buffer
 * @priv_virt:		private data to the buffer representable as
 *			a void *
 * @priv_phys:		private data to the buffer representable as
 *			an ion_phys_addr_t (and someday a phys_addr_t)
 * @lock:		protects the buffers cnt fields
 * @kmap_cnt:		number of times the buffer is mapped to the kernel
 * @vaddr:		the kenrel mapping if kmap_cnt is not zero
 * @dmap_cnt:		number of times the buffer is mapped for dma
 * @sg_table:		the sg table for the buffer if dmap_cnt is not zero
 * @dirty:		bitmask representing which pages of this buffer have
 *			been dirtied by the cpu and need cache maintenance
 *			before dma
 * @vmas:		list of vma's mapping this buffer
 * @handle_count:	count of handles referencing this buffer
 * @task_comm:		taskcomm of last client to reference this buffer in a
 *			handle, used for debugging
 * @pid:		pid of last client to reference this buffer in a
 *			handle, used for debugging
*/
struct ion_buffer {
	struct kref ref;
	struct rb_node node;
	struct ion_device *dev;
	struct ion_heap *heap;
	unsigned long flags;
	size_t size;
	union {
		void *priv_virt;
		ion_phys_addr_t priv_phys;
	};
	struct mutex lock;
	int kmap_cnt;
	void *vaddr;
	int dmap_cnt;
	struct sg_table *sg_table;
	unsigned long *dirty;
	struct list_head vmas;
	/* used to track orphaned buffers */
	int handle_count;
	char task_comm[TASK_COMM_LEN];
	pid_t pid;
};

/**
 * struct ion_heap_ops - ops to operate on a given heap
 * @allocate:		allocate memory
 * @free:		free memory
 * @phys		get physical address of a buffer (only define on
 *			physically contiguous heaps)
 * @map_dma		map the memory for dma to a scatterlist
 * @unmap_dma		unmap the memory for dma
 * @map_kernel		map memory to the kernel
 * @unmap_kernel	unmap memory to the kernel
 * @map_user		map memory to userspace
 */
struct ion_heap_ops {
	int (*allocate) (struct ion_heap *heap,
			 struct ion_buffer *buffer, unsigned long len,
			 unsigned long align, unsigned long flags);
	void (*free) (struct ion_buffer *buffer);
	int (*phys) (struct ion_heap *heap, struct ion_buffer *buffer,
		     ion_phys_addr_t *addr, size_t *len);
	struct sg_table *(*map_dma) (struct ion_heap *heap,
					struct ion_buffer *buffer);
	void (*unmap_dma) (struct ion_heap *heap, struct ion_buffer *buffer);
	void * (*map_kernel) (struct ion_heap *heap, struct ion_buffer *buffer);
	void (*unmap_kernel) (struct ion_heap *heap, struct ion_buffer *buffer);
	int (*map_user) (struct ion_heap *mapper, struct ion_buffer *buffer,
			 struct vm_area_struct *vma);
};

/**
 * struct ion_heap - represents a heap in the system
 * @node:		rb node to put the heap on the device's tree of heaps
 * @dev:		back pointer to the ion_device
 * @type:		type of heap
 * @ops:		ops struct as above
 * @id:			id of heap, also indicates priority of this heap when
 *			allocating.  These are specified by platform data and
 *			MUST be unique
 * @name:		used for debugging
 * @debug_show:		called when heap debug file is read to add any
 *			heap specific debug info to output
 *
 * Represents a pool of memory from which buffers can be made.  In some
 * systems the only heap is regular system memory allocated via vmalloc.
 * On others, some blocks might require large physically contiguous buffers
 * that are allocated from a specially reserved heap.
 */
struct ion_heap {
	struct rb_node node;
	struct ion_device *dev;
	enum ion_heap_type type;
	struct ion_heap_ops *ops;
	int id;
	const char *name;
};

/**
 * functions for creating and destroying the built in ion heaps.
 * architectures can add their own custom architecture specific
 * heaps as appropriate.
 */

struct ion_heap *ion_heap_create(struct ion_platform_heap *);
void ion_heap_destroy(struct ion_heap *);

struct ion_heap *ion_system_heap_create(struct ion_platform_heap *);
void ion_system_heap_destroy(struct ion_heap *);

struct ion_heap *ion_system_contig_heap_create(struct ion_platform_heap *);
void ion_system_contig_heap_destroy(struct ion_heap *);

struct ion_heap *ion_carveout_heap_create(struct ion_platform_heap *);
void ion_carveout_heap_destroy(struct ion_heap *);
/**
 * kernel api to allocate/free from carveout -- used when carveout is
 * used to back an architecture specific custom heap
 */
ion_phys_addr_t ion_carveout_allocate(struct ion_heap *heap, unsigned long size,
				      unsigned long align);
void ion_carveout_free(struct ion_heap *heap, ion_phys_addr_t addr,
		       unsigned long size);
/**
 * The carveout heap returns physical addresses, since 0 may be a valid
 * physical address, this is used to indicate allocation failed
 */
#define ION_CARVEOUT_ALLOCATE_FAIL -1

/**
 * functions for creating and destroying a heap pool -- allows you
 * to keep a pool of pre allocated memory to use from your heap.  Keeping
 * a pool of memory that is ready for dma, ie any cached mapping have been
 * invalidated from the cache, provides a significant peformance benefit on
 * many systems */

/**
 * struct ion_page_pool - pagepool struct
 * @high_count:		number of highmem items in the pool
 * @low_count:		number of lowmem items in the pool
 * @high_items:		list of highmem items
 * @low_items:		list of lowmem items
 * @shrinker:		a shrinker for the items
 * @mutex:		lock protecting this struct and especially the count
 *			item list
 * @alloc:		function to be used to allocate pageory when the pool
 *			is empty
 * @free:		function to be used to free pageory back to the system
 *			when the shrinker fires
 * @gfp_mask:		gfp_mask to use from alloc
 * @order:		order of pages in the pool
 * @list:		plist node for list of pools
 *
 * Allows you to keep a pool of pre allocated pages to use from your heap.
 * Keeping a pool of pages that is ready for dma, ie any cached mapping have
 * been invalidated from the cache, provides a significant peformance benefit
 * on many systems
 */
struct ion_page_pool {
	int high_count;
	int low_count;
	struct list_head high_items;
	struct list_head low_items;
	struct mutex mutex;
	void *(*alloc)(struct ion_page_pool *pool);
	void (*free)(struct ion_page_pool *pool, struct page *page);
	gfp_t gfp_mask;
	unsigned int order;
	struct plist_node list;
};

struct ion_page_pool *ion_page_pool_create(gfp_t gfp_mask, unsigned int order);
void ion_page_pool_destroy(struct ion_page_pool *);
void *ion_page_pool_alloc(struct ion_page_pool *);
void ion_page_pool_free(struct ion_page_pool *, struct page *);


/**
 * struct ion_device - the metadata of the ion device node
 * @dev:		the actual misc device
 * @buffers:		an rb tree of all the existing buffers
 * @buffer_lock:	lock protecting the tree of buffers
 * @lock:		rwsem protecting the tree of heaps and clients
 * @heaps:		list of all the heaps in the system
 * @user_clients:	list of all the clients created from userspace
 */
struct ion_device {
	struct miscdevice dev;
	struct rb_root buffers;
	struct mutex buffer_lock;
	struct rw_semaphore lock;
	struct rb_root heaps;
	long (*custom_ioctl) (struct ion_client *client, unsigned int cmd,
			      unsigned long arg);
	struct rb_root clients;
};

/**
 * struct ion_client - a process/hw block local address space
 * @node:		node in the tree of all clients
 * @dev:		backpointer to ion device
 * @handles:		an rb tree of all the handles in this client
 * @lock:		lock protecting the tree of handles
 * @heap_mask:		mask of all supported heaps
 * @name:		used for debugging
 * @task:		used for debugging
 *
 * A client represents a list of buffers this client may access.
 * The mutex stored here is used to protect both handles tree
 * as well as the handles themselves, and should be held while modifying either.
 */
struct ion_client {
	struct rb_node node;
	struct ion_device *dev;
	struct rb_root handles;
	struct mutex lock;
	unsigned int heap_mask;
	const char *name;
	struct task_struct *task;
	pid_t pid;
};

/**
 * ion_handle - a client local reference to a buffer
 * @ref:		reference count
 * @client:		back pointer to the client the buffer resides in
 * @buffer:		pointer to the buffer
 * @node:		node in the client's handle rbtree
 * @kmap_cnt:		count of times this client has mapped to kernel
 * @dmap_cnt:		count of times this client has mapped for dma
 *
 * Modifications to node, map_cnt or mapping should be protected by the
 * lock in the client.  Other fields are never changed after initialization.
 */
struct ion_handle {
	struct kref ref;
	struct ion_client *client;
	struct ion_buffer *buffer;
	struct rb_node node;
	unsigned int kmap_cnt;
};


static bool ion_buffer_fault_user_mappings(struct ion_buffer *buffer)
{
        return ((buffer->flags & ION_FLAG_CACHED) &&
                !(buffer->flags & ION_FLAG_CACHED_NEEDS_SYNC));
}


/* this function should only be called while dev->lock is held */
static void ion_buffer_add(struct ion_device *dev,
			   struct ion_buffer *buffer)
{
	struct rb_node **p = &dev->buffers.rb_node;
	struct rb_node *parent = NULL;
	struct ion_buffer *entry;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct ion_buffer, node);

		if (buffer < entry) {
			p = &(*p)->rb_left;
		} else if (buffer > entry) {
			p = &(*p)->rb_right;
		} else {
			pr_err("%s: buffer already found.", __func__);
			BUG();
		}
	}

	rb_link_node(&buffer->node, parent, p);
	rb_insert_color(&buffer->node, &dev->buffers);
}

static int ion_buffer_alloc_dirty(struct ion_buffer *buffer);

/* this function should only be called while dev->lock is held */
static struct ion_buffer *ion_buffer_create(struct ion_heap *heap,
				     struct ion_device *dev,
				     unsigned long len,
				     unsigned long align,
				     unsigned long flags)
{
	struct ion_buffer *buffer;
	struct sg_table *table;
	struct scatterlist *sg;
	int i, ret;

	buffer = kzalloc(sizeof(struct ion_buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	buffer->heap = heap;
	buffer->flags = flags;
	kref_init(&buffer->ref);

	ret = heap->ops->allocate(heap, buffer, len, align, flags);
	if (ret) {
		kfree(buffer);
		return ERR_PTR(ret);
	}

	buffer->dev = dev;
	buffer->size = len;

	table = heap->ops->map_dma(heap, buffer);
	if (IS_ERR_OR_NULL(table)) {
		heap->ops->free(buffer);
		kfree(buffer);
		return ERR_PTR(PTR_ERR(table));
	}
	buffer->sg_table = table;
	if (ion_buffer_fault_user_mappings(buffer)) {
		for_each_sg(buffer->sg_table->sgl, sg, buffer->sg_table->nents,
			    i) {
			if (sg_dma_len(sg) == PAGE_SIZE)
				continue;
			pr_err("%s: cached mappings that will be faulted in "
			       "must have pagewise sg_lists\n", __func__);
			ret = -EINVAL;
			goto err;
		}

		ret = ion_buffer_alloc_dirty(buffer);
		if (ret)
			goto err;
	}

	buffer->dev = dev;
	buffer->size = len;
	INIT_LIST_HEAD(&buffer->vmas);
	mutex_init(&buffer->lock);
	/* this will set up dma addresses for the sglist -- it is not
	   technically correct as per the dma api -- a specific
	   device isn't really taking ownership here.  However, in practice on
	   our systems the only dma_address space is physical addresses.
	   Additionally, we can't afford the overhead of invalidating every
	   allocation via dma_map_sg. The implicit contract here is that
	   memory comming from the heaps is ready for dma, ie if it has a
	   cached mapping that mapping has been invalidated */
	for_each_sg(buffer->sg_table->sgl, sg, buffer->sg_table->nents, i)
		sg_dma_address(sg) = sg_phys(sg);
	mutex_lock(&dev->buffer_lock);
	ion_buffer_add(dev, buffer);
	mutex_unlock(&dev->buffer_lock);
	return buffer;

err:
	heap->ops->unmap_dma(heap, buffer);
	heap->ops->free(buffer);
	kfree(buffer);
	return ERR_PTR(ret);
}

static void ion_buffer_destroy(struct kref *kref)
{
	struct ion_buffer *buffer = container_of(kref, struct ion_buffer, ref);
	struct ion_device *dev = buffer->dev;

	if (WARN_ON(buffer->kmap_cnt > 0))
		buffer->heap->ops->unmap_kernel(buffer->heap, buffer);
	buffer->heap->ops->unmap_dma(buffer->heap, buffer);
	buffer->heap->ops->free(buffer);
	mutex_lock(&dev->buffer_lock);
	rb_erase(&buffer->node, &dev->buffers);
	mutex_unlock(&dev->buffer_lock);
	if (buffer->flags & ION_FLAG_CACHED)
		kfree(buffer->dirty);
	kfree(buffer);
}

static void ion_buffer_get(struct ion_buffer *buffer)
{
	kref_get(&buffer->ref);
}

static int ion_buffer_put(struct ion_buffer *buffer)
{
	return kref_put(&buffer->ref, ion_buffer_destroy);
}

static void ion_buffer_add_to_handle(struct ion_buffer *buffer)
{
	mutex_lock(&buffer->lock);
	buffer->handle_count++;
	mutex_unlock(&buffer->lock);
}

static void ion_buffer_remove_from_handle(struct ion_buffer *buffer)
{
	/*
	 * when a buffer is removed from a handle, if it is not in
	 * any other handles, copy the taskcomm and the pid of the
	 * process it's being removed from into the buffer.  At this
	 * point there will be no way to track what processes this buffer is
	 * being used by, it only exists as a dma_buf file descriptor.
	 * The taskcomm and pid can provide a debug hint as to where this fd
	 * is in the system
	 */
	mutex_lock(&buffer->lock);
	buffer->handle_count--;
	BUG_ON(buffer->handle_count < 0);
	if (!buffer->handle_count) {
		struct task_struct *task;

		task = current->group_leader;
		get_task_comm(buffer->task_comm, task);
		buffer->pid = task_pid_nr(task);
	}
	mutex_unlock(&buffer->lock);
}

static struct ion_handle *ion_handle_create(struct ion_client *client,
				     struct ion_buffer *buffer)
{
	struct ion_handle *handle;

	handle = kzalloc(sizeof(struct ion_handle), GFP_KERNEL);
	if (!handle)
		return ERR_PTR(-ENOMEM);
	kref_init(&handle->ref);
	rb_init_node(&handle->node);
	handle->client = client;
	ion_buffer_get(buffer);
	ion_buffer_add_to_handle(buffer);
	handle->buffer = buffer;

	return handle;
}

static void ion_handle_kmap_put(struct ion_handle *);

static void ion_handle_destroy(struct kref *kref)
{
	struct ion_handle *handle = container_of(kref, struct ion_handle, ref);
	struct ion_client *client = handle->client;
	struct ion_buffer *buffer = handle->buffer;

	mutex_lock(&buffer->lock);
	while (handle->kmap_cnt)
		ion_handle_kmap_put(handle);
	mutex_unlock(&buffer->lock);

	if (!RB_EMPTY_NODE(&handle->node))
		rb_erase(&handle->node, &client->handles);

	ion_buffer_remove_from_handle(buffer);
	ion_buffer_put(buffer);

	kfree(handle);
}


static bool ion_handle_validate(struct ion_client *client, struct ion_handle *handle)
{
	struct rb_node *n = client->handles.rb_node;

	while (n) {
		struct ion_handle *handle_node = rb_entry(n, struct ion_handle,
							  node);
		if (handle < handle_node)
			n = n->rb_left;
		else if (handle > handle_node)
			n = n->rb_right;
		else
			return true;
	}
	return false;
}

static void ion_handle_add(struct ion_client *client, struct ion_handle *handle)
{
	struct rb_node **p = &client->handles.rb_node;
	struct rb_node *parent = NULL;
	struct ion_handle *entry;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct ion_handle, node);

		if (handle < entry)
			p = &(*p)->rb_left;
		else if (handle > entry)
			p = &(*p)->rb_right;
		else
			WARN(1, "%s: buffer already found.", __func__);
	}

	rb_link_node(&handle->node, parent, p);
	rb_insert_color(&handle->node, &client->handles);
}

static struct ion_handle *ion_alloc(struct ion_client *client, size_t len,
				    size_t align, unsigned int heap_mask,
				    unsigned int flags)
{
	struct rb_node *n;
	struct ion_handle *handle;
	struct ion_device *dev = client->dev;
	struct ion_buffer *buffer = NULL;

	pr_debug("%s: len %d align %d heap_mask %u flags %x\n", __func__, len,
		 align, heap_mask, flags);
	/*
	 * traverse the list of heaps available in this system in priority
	 * order.  If the heap type is supported by the client, and matches the
	 * request of the caller allocate from it.  Repeat until allocate has
	 * succeeded or all heaps have been tried
	 */
	if (WARN_ON(!len))
		return ERR_PTR(-EINVAL);

	len = PAGE_ALIGN(len);

	down_read(&dev->lock);
	for (n = rb_first(&dev->heaps); n != NULL; n = rb_next(n)) {
		struct ion_heap *heap = rb_entry(n, struct ion_heap, node);
		/* if the client doesn't support this heap type */
		if (!((1 << heap->type) & client->heap_mask))
			continue;
		/* if the caller didn't specify this heap type */
		if (!((1 << heap->id) & heap_mask))
			continue;
		buffer = ion_buffer_create(heap, dev, len, align, flags);
		if (!IS_ERR_OR_NULL(buffer))
			break;
	}
	up_read(&dev->lock);

	if (buffer == NULL)
		return ERR_PTR(-ENODEV);

	if (IS_ERR(buffer))
		return ERR_PTR(PTR_ERR(buffer));

	handle = ion_handle_create(client, buffer);

	/*
	 * ion_buffer_create will create a buffer with a ref_cnt of 1,
	 * and ion_handle_create will take a second reference, drop one here
	 */
	ion_buffer_put(buffer);

	if (!IS_ERR(handle)) {
		mutex_lock(&client->lock);
		ion_handle_add(client, handle);
		mutex_unlock(&client->lock);
	}


	return handle;
}



static void *ion_buffer_kmap_get(struct ion_buffer *buffer)
{
	void *vaddr;

	if (buffer->kmap_cnt) {
		buffer->kmap_cnt++;
		return buffer->vaddr;
	}
	vaddr = buffer->heap->ops->map_kernel(buffer->heap, buffer);
	if (IS_ERR_OR_NULL(vaddr))
		return vaddr;
	buffer->vaddr = vaddr;
	buffer->kmap_cnt++;
	return vaddr;
}


static void ion_buffer_kmap_put(struct ion_buffer *buffer)
{
	buffer->kmap_cnt--;
	if (!buffer->kmap_cnt) {
		buffer->heap->ops->unmap_kernel(buffer->heap, buffer);
		buffer->vaddr = NULL;
	}
}

static void ion_handle_kmap_put(struct ion_handle *handle)
{
	struct ion_buffer *buffer = handle->buffer;

	handle->kmap_cnt--;
	if (!handle->kmap_cnt)
		ion_buffer_kmap_put(buffer);
}



static struct ion_client *ion_client_create(struct ion_device *dev,
					    unsigned int heap_mask,
					    const char *name)
{
	struct ion_client *client;
	struct task_struct *task;
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct ion_client *entry;
	char debug_name[64];
	pid_t pid;

	get_task_struct(current->group_leader);
	task_lock(current->group_leader);
	pid = task_pid_nr(current->group_leader);
	/* don't bother to store task struct for kernel threads,
	   they can't be killed anyway */
	if (current->group_leader->flags & PF_KTHREAD) {
		put_task_struct(current->group_leader);
		task = NULL;
	} else {
		task = current->group_leader;
	}
	task_unlock(current->group_leader);

	client = kzalloc(sizeof(struct ion_client), GFP_KERNEL);
	if (!client) {
		if (task)
			put_task_struct(current->group_leader);
		return ERR_PTR(-ENOMEM);
	}

	client->dev = dev;
	client->handles = RB_ROOT;
	mutex_init(&client->lock);
	client->name = name;
	client->heap_mask = heap_mask;
	client->task = task;
	client->pid = pid;

	down_write(&dev->lock);
	p = &dev->clients.rb_node;
	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct ion_client, node);

		if (client < entry)
			p = &(*p)->rb_left;
		else if (client > entry)
			p = &(*p)->rb_right;
	}
	rb_link_node(&client->node, parent, p);
	rb_insert_color(&client->node, &dev->clients);

	snprintf(debug_name, 64, "%u", client->pid);
	up_write(&dev->lock);

	return client;
}

static void ion_client_destroy(struct ion_client *client)
{
	struct ion_device *dev = client->dev;
	struct rb_node *n;

	pr_debug("%s: %d\n", __func__, __LINE__);
	while ((n = rb_first(&client->handles))) {
		struct ion_handle *handle = rb_entry(n, struct ion_handle,
						     node);
		ion_handle_destroy(&handle->ref);
	}
	down_write(&dev->lock);
	if (client->task)
		put_task_struct(client->task);
	rb_erase(&client->node, &dev->clients);
	up_write(&dev->lock);

	kfree(client);
}


static void ion_buffer_sync_for_device(struct ion_buffer *buffer,
				       struct device *dev,
				       enum dma_data_direction direction);

static struct sg_table *ion_map_dma_buf(struct dma_buf_attachment *attachment,
					enum dma_data_direction direction)
{
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct ion_buffer *buffer = dmabuf->priv;

	ion_buffer_sync_for_device(buffer, attachment->dev, direction);
	return buffer->sg_table;
}

static void ion_unmap_dma_buf(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction direction)
{
}

static int ion_buffer_alloc_dirty(struct ion_buffer *buffer)
{
	unsigned long pages = buffer->sg_table->nents;
	unsigned long length = (pages + BITS_PER_LONG - 1)/BITS_PER_LONG;

	buffer->dirty = kzalloc(length * sizeof(unsigned long), GFP_KERNEL);
	if (!buffer->dirty)
		return -ENOMEM;
	return 0;
}

struct ion_vma_list {
	struct list_head list;
	struct vm_area_struct *vma;
};

static void ion_buffer_sync_for_device(struct ion_buffer *buffer,
				       struct device *dev,
				       enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;
	struct ion_vma_list *vma_list;

	pr_debug("%s: syncing for device %s\n", __func__,
		 dev ? dev_name(dev) : "null");

	if (!ion_buffer_fault_user_mappings(buffer))
		return;

	mutex_lock(&buffer->lock);
	for_each_sg(buffer->sg_table->sgl, sg, buffer->sg_table->nents, i) {
		if (!test_bit(i, buffer->dirty))
			continue;
		dma_sync_sg_for_device(dev, sg, 1, dir);
		clear_bit(i, buffer->dirty);
	}
	list_for_each_entry(vma_list, &buffer->vmas, list) {
		struct vm_area_struct *vma = vma_list->vma;

		zap_page_range(vma, vma->vm_start, vma->vm_end - vma->vm_start,
			       NULL);
	}
	mutex_unlock(&buffer->lock);
}

static int ion_vm_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct ion_buffer *buffer = vma->vm_private_data;
	struct scatterlist *sg;
	int i;

	mutex_lock(&buffer->lock);
	set_bit(vmf->pgoff, buffer->dirty);

	for_each_sg(buffer->sg_table->sgl, sg, buffer->sg_table->nents, i) {
		if (i != vmf->pgoff)
			continue;
		dma_sync_sg_for_cpu(NULL, sg, 1, DMA_BIDIRECTIONAL);
		vm_insert_page(vma, (unsigned long)vmf->virtual_address,
			       sg_page(sg));
		break;
	}
	mutex_unlock(&buffer->lock);
	return VM_FAULT_NOPAGE;
}

static void ion_vm_open(struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = vma->vm_private_data;
	struct ion_vma_list *vma_list;

	vma_list = kmalloc(sizeof(struct ion_vma_list), GFP_KERNEL);
	if (!vma_list)
		return;
	vma_list->vma = vma;
	mutex_lock(&buffer->lock);
	list_add(&vma_list->list, &buffer->vmas);
	mutex_unlock(&buffer->lock);
	pr_debug("%s: adding %p\n", __func__, vma);
}

static void ion_vm_close(struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = vma->vm_private_data;
	struct ion_vma_list *vma_list, *tmp;

	pr_debug("%s\n", __func__);
	mutex_lock(&buffer->lock);
	list_for_each_entry_safe(vma_list, tmp, &buffer->vmas, list) {
		if (vma_list->vma != vma)
			continue;
		list_del(&vma_list->list);
		kfree(vma_list);
		pr_debug("%s: deleting %p\n", __func__, vma);
		break;
	}
	mutex_unlock(&buffer->lock);
}

static struct vm_operations_struct ion_vma_ops = {
	.open = ion_vm_open,
	.close = ion_vm_close,
	.fault = ion_vm_fault,
};

static int ion_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = dmabuf->priv;
	int ret = 0;

	if (!buffer->heap->ops->map_user) {
		pr_err("%s: this heap does not define a method for mapping "
		       "to userspace\n", __func__);
		return -EINVAL;
	}

	if (ion_buffer_fault_user_mappings(buffer)) {
		vma->vm_private_data = buffer;
		vma->vm_ops = &ion_vma_ops;
		ion_vm_open(vma);
		return 0;
	}

	if (!(buffer->flags & ION_FLAG_CACHED))
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	mutex_lock(&buffer->lock);
	/* now map it to userspace */
	ret = buffer->heap->ops->map_user(buffer->heap, buffer, vma);
	mutex_unlock(&buffer->lock);

	if (ret)
		pr_err("%s: failure mapping buffer to userspace\n",
		       __func__);

	return ret;
}

static void ion_dma_buf_release(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = dmabuf->priv;
	ion_buffer_put(buffer);
}

static void *ion_dma_buf_kmap(struct dma_buf *dmabuf, unsigned long offset)
{
	struct ion_buffer *buffer = dmabuf->priv;
	return buffer->vaddr + offset * PAGE_SIZE;
}

static void ion_dma_buf_kunmap(struct dma_buf *dmabuf, unsigned long offset,
			       void *ptr)
{
	return;
}

static int ion_dma_buf_begin_cpu_access(struct dma_buf *dmabuf, size_t start,
					size_t len,
					enum dma_data_direction direction)
{
	struct ion_buffer *buffer = dmabuf->priv;
	void *vaddr;

	if (!buffer->heap->ops->map_kernel) {
		pr_err("%s: map kernel is not implemented by this heap.\n",
		       __func__);
		return -ENODEV;
	}

	mutex_lock(&buffer->lock);
	vaddr = ion_buffer_kmap_get(buffer);
	mutex_unlock(&buffer->lock);
	if (IS_ERR(vaddr))
		return PTR_ERR(vaddr);
	if (!vaddr)
		return -ENOMEM;
	return 0;
}

static void ion_dma_buf_end_cpu_access(struct dma_buf *dmabuf, size_t start,
				       size_t len,
				       enum dma_data_direction direction)
{
	struct ion_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	ion_buffer_kmap_put(buffer);
	mutex_unlock(&buffer->lock);
}

static struct dma_buf_ops dma_buf_ops = {
	.map_dma_buf = ion_map_dma_buf,
	.unmap_dma_buf = ion_unmap_dma_buf,
	.mmap = ion_mmap,
	.release = ion_dma_buf_release,
	.begin_cpu_access = ion_dma_buf_begin_cpu_access,
	.end_cpu_access = ion_dma_buf_end_cpu_access,
	.kmap_atomic = ion_dma_buf_kmap,
	.kunmap_atomic = ion_dma_buf_kunmap,
	.kmap = ion_dma_buf_kmap,
	.kunmap = ion_dma_buf_kunmap,
};

static int ion_share_dma_buf(struct ion_client *client, struct ion_handle *handle)
{
	struct ion_buffer *buffer;
	struct dma_buf *dmabuf;
	bool valid_handle;
	int fd;

	mutex_lock(&client->lock);
	valid_handle = ion_handle_validate(client, handle);
	mutex_unlock(&client->lock);
	if (!valid_handle) {
		WARN(1, "%s: invalid handle passed to share.\n", __func__);
		return -EINVAL;
	}

	buffer = handle->buffer;
	ion_buffer_get(buffer);
	dmabuf = dma_buf_export(buffer, &dma_buf_ops, buffer->size, O_RDWR);
	if (IS_ERR(dmabuf)) {
		ion_buffer_put(buffer);
		return PTR_ERR(dmabuf);
	}
	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);

	return fd;
}


static int ion_release(struct inode *inode, struct file *file)
{
	struct ion_client *client = file->private_data;

	pr_debug("%s: %d\n", __func__, __LINE__);
	ion_client_destroy(client);
	return 0;
}

static int ion_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct ion_device *dev = container_of(miscdev, struct ion_device, dev);
	struct ion_client *client;

	pr_debug("%s: %d\n", __func__, __LINE__);
	client = ion_client_create(dev, -1, "user");
	if (IS_ERR_OR_NULL(client))
		return PTR_ERR(client);
	file->private_data = client;

	return 0;
}

static const struct file_operations ion_fops = {
	.owner          = THIS_MODULE,
	.open           = ion_open,
	.release        = ion_release,
};


static void ion_device_add_heap(struct ion_device *dev, struct ion_heap *heap)
{
	struct rb_node **p = &dev->heaps.rb_node;
	struct rb_node *parent = NULL;
	struct ion_heap *entry;

	if (!heap->ops->allocate || !heap->ops->free || !heap->ops->map_dma ||
	    !heap->ops->unmap_dma)
		pr_err("%s: can not add heap with invalid ops struct.\n",
		       __func__);

	heap->dev = dev;
	down_write(&dev->lock);
	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct ion_heap, node);

		if (heap->id < entry->id) {
			p = &(*p)->rb_left;
		} else if (heap->id > entry->id ) {
			p = &(*p)->rb_right;
		} else {
			pr_err("%s: can not insert multiple heaps with "
				"id %d\n", __func__, heap->id);
			goto end;
		}
	}

	rb_link_node(&heap->node, parent, p);
	rb_insert_color(&heap->node, &dev->heaps);
end:
	up_write(&dev->lock);
}

static struct ion_device *ion_device_create(long (*custom_ioctl)
					    (struct ion_client *client,
					     unsigned int cmd,
					     unsigned long arg))
{
	struct ion_device *idev;
	int ret;

	idev = kzalloc(sizeof(struct ion_device), GFP_KERNEL);
	if (!idev)
		return ERR_PTR(-ENOMEM);

	idev->dev.minor = MISC_DYNAMIC_MINOR;
	idev->dev.name = "ion";
	idev->dev.fops = &ion_fops;
	idev->dev.parent = NULL;
	ret = misc_register(&idev->dev);
	if (ret) {
		pr_err("ion: failed to register misc device.\n");
		return ERR_PTR(ret);
	}
	idev->custom_ioctl = custom_ioctl;
	idev->buffers = RB_ROOT;
	mutex_init(&idev->buffer_lock);
	init_rwsem(&idev->lock);
	idev->heaps = RB_ROOT;
	idev->clients = RB_ROOT;
	return idev;
}

static void ion_device_destroy(struct ion_device *dev)
{
	misc_deregister(&dev->dev);
	/* XXX need to free the heaps and clients ? */
	kfree(dev);
}

//
/////////////////////////////////////////////////////////////


struct ion_device *portal_ion_device;
struct ion_heap *portal_ion_heap[2]; 

void portal_init_ion(void)
{
        struct ion_platform_heap heap_data;
        int i;
        heap_data.type = ION_HEAP_TYPE_SYSTEM_CONTIG;
        // fixme use devicetree
        heap_data.base = 0x18000000; // not used for system_contig or system
        heap_data.size = 0x08000000; // not used for system_contig or system

        if (portal_ion_device == NULL) {
                printk("creating ion_device for portal\n");
                portal_ion_device = ion_device_create(NULL);
                printk("ion_device %p\n", portal_ion_device);
                for (i = 0; i < 2; i++) {
                        char name[22];
                        snprintf(name, sizeof(name), "portal-ion-heap-%d", i);

                        heap_data.id = i;
                        heap_data.name = name;
                        printk("creating ion_heap for portal\n");
                        portal_ion_heap[i] = ion_heap_create(&heap_data);
                        printk("ion_heap %p\n", portal_ion_heap);
                        ion_device_add_heap(portal_ion_device, portal_ion_heap[i]);

                        // next one is system
                        heap_data.type = ION_HEAP_TYPE_SYSTEM;
                }
        }
}

void portal_ion_release(void)
{
        int i;

        ion_device_destroy(portal_ion_device);
        portal_ion_device = NULL;
        for (i = 0; i < 2; i++) {
                ion_heap_destroy(portal_ion_heap[i]);
                portal_ion_heap[i] = NULL;
        }
}

static void dump_ind_regs(const char *prefix, struct portal_data *portal_data)
{
        int i;
        for (i = 0; i < 10; i++) {
                unsigned long regval;
                regval = readl(portal_data->ind_reg_base_virt + i*4);
                driver_devel("%s reg %x value %08lx\n", prefix,
                             i*4, regval);
        }
}

static irqreturn_t portal_isr(int irq, void *dev_id)
{
	struct portal_data *portal_data = (struct portal_data *)dev_id;
	u32 int_src, int_en;


        //dump_ind_regs("ISR a", portal_data);
        int_src = readl(portal_data->ind_reg_base_virt + 0);
	int_en  = readl(portal_data->ind_reg_base_virt + 4);
	driver_devel("%s IRQ %s %d %x %x\n", __func__, portal_data->device_name, irq, int_src, int_en);

	// disable interrupt.  this will be enabled by user mode 
	// driver  after all the HW->SW FIFOs have been emptied
        writel(0, portal_data->ind_reg_base_virt + 0x4);

        //dump_ind_regs("ISR b", portal_data);
        mutex_unlock(&portal_data->completion_mutex);
	wake_up_interruptible(&portal_data->wait_queue);

        return IRQ_HANDLED;
}

static int portal_open(struct inode *inode, struct file *filep)
{
	struct miscdevice *miscdev = filep->private_data;
	struct portal_data *portal_data =
                container_of(miscdev, struct portal_data, misc);
        struct portal_client *portal_client =
                (struct portal_client *)kzalloc(sizeof(struct portal_client), GFP_KERNEL);

        driver_devel("%s: %s ind_reg_base_phys %lx ind_fifo_base_phys %lx\n", __FUNCTION__, portal_data->device_name,
                     (long)portal_data->ind_reg_base_phys, (long)(portal_data->ind_fifo_base_phys));
        driver_devel("%s: %s req_reg_base_phys %lx req_fifo_base_phys %lx\n", __FUNCTION__, portal_data->device_name,
                     (long)portal_data->req_reg_base_phys, (long)(portal_data->req_fifo_base_phys));

        dump_ind_regs("portal_open", portal_data);

        portal_client->ion_client = ion_client_create(portal_ion_device, 0xf, "portal_ion_client");
        portal_client->portal_data = portal_data;
        printk("portal created ion_client %p\n", portal_client->ion_client);
        filep->private_data = portal_client;

        // clear indication status (ignored by HW)
        writel(0, portal_data->ind_reg_base_virt + 0);
        // enable indication interrupts
        writel(1, portal_data->ind_reg_base_virt + 4);

	// sanity check, see if interrupts have been enabled
        dump_ind_regs("enable interrupts", portal_data);

	return 0;
}


long portal_unlocked_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;
        switch (cmd) {
	case PORTAL_DCACHE_FLUSH_INVAL: {
	  struct PortalAlloc alloc;
	  int i;
	  if (copy_from_user(&alloc, (void __user *)arg, sizeof(alloc)))
	    return -EFAULT;
	  //printk("portal_dcache_flush_inval\n");
	  for(i = 0; i < alloc.numEntries; i++){
	    unsigned int start_addr = alloc.entries[i].dma_address;
	    unsigned int end_addr = start_addr + alloc.entries[i].length;
	    // printk("portal_dcache_flush_inval[%d] %08x %d\n", i, start_addr, end_addr);
	    // we saw this funciton invoked in arch/arm/mm/dma-mapping.c it works on physical addresses.
	    outer_clean_range(start_addr, end_addr);
	    outer_inv_range(start_addr, end_addr);
	  }
	  return 0;
	}
        case PORTAL_ALLOC: {
                struct PortalAlloc alloc;
                struct dma_buf *dma_buf = 0;
                struct dma_buf_attachment *attachment = 0;
                struct sg_table *sg_table = 0;
                struct scatterlist *sg;
		struct ion_handle* handle;
                int i;

		if (copy_from_user(&alloc, (void __user *)arg, sizeof(alloc)))
			return -EFAULT;
                alloc.size = round_up(alloc.size, 4096);
                handle = ion_alloc(portal_client->ion_client, alloc.size, 4096, 0xf, 0);
                printk("allocated ion_handle %p size %d\n", handle, alloc.size);
                if (IS_ERR_VALUE((long)handle))
                        return -EINVAL;
                alloc.fd = ion_share_dma_buf(portal_client->ion_client, handle);
                dma_buf = dma_buf_get(alloc.fd);
                attachment = dma_buf_attach(dma_buf, portal_client->portal_data->misc.this_device);
                sg_table = dma_buf_map_attachment(attachment, DMA_TO_DEVICE);
                printk("sg_table %p nents %d\n", sg_table, sg_table->nents);
                if (sg_table->nents > 1) {
                        printk("sg_is_chain=%ld sg_is_last=%ld\n",
                               sg_is_chain(sg_table->sgl), sg_is_last(sg_table->sgl));
                        for_each_sg(sg_table->sgl, sg, sg_table->nents, i) {
                                printk("sg[%d] sg=%p phys=%lx offset=%08x length=%x\n",
                                       i, sg, (long)sg_phys(sg), sg->offset, sg->length);
                        }
                }
                
                memset(&alloc.entries, 0, sizeof(alloc.entries));
                alloc.numEntries = sg_table->nents;
                for_each_sg(sg_table->sgl, sg, sg_table->nents, i) {
                        alloc.entries[i].dma_address = sg_phys(sg);
                        alloc.entries[i].length = sg->length;
                }

                //sg_free_table(sg_table);
                //dma_buf_detach(dma_buf, attachment);

                if (copy_to_user((void __user *)arg, &alloc, sizeof(alloc)))
                        return -EFAULT;
                return 0;
        } break;
	case PORTAL_SET_FCLK_RATE: {
		PortalClockRequest request;
		char clkname[8];
		int status = 0;
		struct clk *fclk = NULL;

		if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
			return -EFAULT;

		snprintf(clkname, sizeof(clkname), "FPGA%d", request.clknum);
		fclk = clk_get_sys(clkname, NULL);
		printk(KERN_INFO "[%s:%d] fclk %s %p\n", __FUNCTION__, __LINE__, clkname, fclk);
		if (!fclk)
			return -ENODEV;
		request.actual_rate = clk_round_rate(fclk, request.requested_rate);
		printk(KERN_INFO "[%s:%d] requested rate %ld actual rate %ld\n", __FUNCTION__, __LINE__, request.requested_rate, request.actual_rate);
		if ((status = clk_set_rate(fclk, request.actual_rate))) {
			printk(KERN_INFO "[%s:%d] err\n", __FUNCTION__, __LINE__);
			return status;
		}
                if (copy_to_user((void __user *)arg, &request, sizeof(request)))
                        return -EFAULT;
		return status;
	} break;
        default:
                printk("portal_unlocked_ioctl ENOTTY cmd=%x\n", cmd);
                return -ENOTTY;
        }

        return -ENODEV;
}

int portal_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;
	unsigned long off = portal_data->dev_base_phys;
	unsigned long req_len = vma->vm_end - vma->vm_start + (vma->vm_pgoff << PAGE_SHIFT);

        if (!portal_client)
                return -ENODEV;
        if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
                return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	vma->vm_pgoff = off >> PAGE_SHIFT;
	vma->vm_flags |= VM_IO | VM_RESERVED;
        if (io_remap_pfn_range(vma, vma->vm_start, off >> PAGE_SHIFT,
                               vma->vm_end - vma->vm_start, vma->vm_page_prot))
                return -EAGAIN;

        printk("%s req_len=%lx off=%lx\n", __FUNCTION__, req_len, off);
	if(0)
	  dump_ind_regs(__FUNCTION__, portal_data);

        return 0;
}


unsigned int portal_poll (struct file *filep, poll_table *poll_table)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;
        int int_status = readl(portal_data->ind_reg_base_virt + 0);
        int mask = 0;
        poll_wait(filep, &portal_data->wait_queue, poll_table);
        if (int_status & 1)
                mask = POLLIN | POLLRDNORM;
	if(0)
        printk("%s: %s int_status=%x mask=%x\n", __FUNCTION__, portal_data->device_name, int_status, mask);
        return mask;
}

static int portal_release(struct inode *inode, struct file *filep)
{
	struct portal_client *portal_client = filep->private_data;
	driver_devel("%s inode=%p filep=%p\n", __func__, inode, filep);
        ion_client_destroy(portal_client->ion_client);
        kfree(portal_client);
        return 0;
}

static const struct file_operations portal_fops = {
	.open = portal_open,
        .mmap = portal_mmap,
        .unlocked_ioctl = portal_unlocked_ioctl,
        .poll = portal_poll,
	.release = portal_release,
};

int portal_init_driver(struct portal_init_data *init_data)
{
	struct device *dev;
	struct portal_data *portal_data;
	struct resource *reg_res, *irq_res;
        struct miscdevice *miscdev;
	int rc = 0, dev_range=0, reg_range=0, fifo_range=0;
	driver_devel("%s\n", __func__);
	driver_devel("%s relies on a custom modification to arch/arm/mm/cache-v7.S:ENTRY(v7_coherent_user_range)", __func__);
	dev = &init_data->pdev->dev;

        if (!portal_ion_device)
                portal_init_ion();

	reg_res = platform_get_resource(init_data->pdev, IORESOURCE_MEM, 0);
	irq_res = platform_get_resource(init_data->pdev, IORESOURCE_IRQ, 0);
	if ((!reg_res) || (!irq_res)) {
		pr_err("Error portal resources\n");
		return -ENODEV;
	}

	portal_data = kzalloc(sizeof(struct portal_data), GFP_KERNEL);
	if (!portal_data) {
		pr_err("Error portal allocating internal data\n");
		rc = -ENOMEM;
		goto err_mem;
	}
        portal_data->device_name = init_data->device_name;
        portal_data->dev_base_phys = reg_res->start;
        portal_data->ind_reg_base_phys = reg_res->start + (3 << 14);
	portal_data->ind_fifo_base_phys = reg_res->start + (2 << 14);
        portal_data->req_reg_base_phys = reg_res->start + (1 << 14);
	portal_data->req_fifo_base_phys = reg_res->start + (0 << 14);
	
	dev_range = reg_res->end - reg_res->start;
	fifo_range = 1 << 14;
	reg_range = 1 << 14;

	portal_data->dev_base_virt = ioremap_nocache(portal_data->dev_base_phys, dev_range);
        portal_data->ind_reg_base_virt = ioremap_nocache(portal_data->ind_reg_base_phys, reg_range);
        portal_data->ind_fifo_base_virt = ioremap_nocache(portal_data->ind_fifo_base_phys, fifo_range);
        portal_data->req_reg_base_virt = ioremap_nocache(portal_data->req_reg_base_phys, reg_range);
        portal_data->req_fifo_base_virt = ioremap_nocache(portal_data->req_fifo_base_phys, fifo_range);

        pr_info("%s ind_reg_base phys %x/%x virt %p\n",
                portal_data->device_name,
                portal_data->ind_reg_base_phys, reg_range, portal_data->ind_reg_base_virt);

        pr_info("%s ind_fifo_base phys %x/%x virt %p\n",
                portal_data->device_name,
                portal_data->ind_fifo_base_phys, fifo_range, portal_data->ind_fifo_base_virt);

        mutex_init(&portal_data->completion_mutex);
        mutex_lock(&portal_data->completion_mutex);
        init_waitqueue_head(&portal_data->wait_queue);

	portal_data->portal_irq = irq_res->start;
	if (request_irq(portal_data->portal_irq, portal_isr,
			IRQF_TRIGGER_HIGH, portal_data->device_name, portal_data)) {
		portal_data->portal_irq = 0;
		goto err_bb;
	}

	portal_data->dev = dev;
	dev_set_drvdata(dev, (void *)portal_data);

        miscdev = &portal_data->misc;
        driver_devel("%s:%d miscdev=%p\n", __func__, __LINE__, miscdev);
        driver_devel("%s:%d portal_data=%p\n", __func__, __LINE__, portal_data);
        miscdev->minor = MISC_DYNAMIC_MINOR;
        miscdev->name = portal_data->device_name;
        miscdev->fops = &portal_fops;
        miscdev->parent = NULL;
        misc_register(miscdev);

	return 0;

err_bb:
	if (portal_data->portal_irq != 0)
		free_irq(portal_data->portal_irq, portal_data);

err_mem:
	if (portal_data) {
		kfree(portal_data);
	}

	dev_set_drvdata(dev, NULL);

	return rc;
}

int portal_deinit_driver(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct portal_data *portal_data = 
                (struct portal_data *)dev_get_drvdata(dev);

	driver_devel("%s\n", __func__);

	free_irq(portal_data->portal_irq, portal_data);
	kfree(portal_data);

	dev_set_drvdata(dev, NULL);

	return 0;
}

static int portal_parse_hw_info(struct device_node *np,
                                     struct portal_init_data *init_data)
{
	u32 const *prop;
	int size;

	prop = of_get_property(np, "device-name", &size);
	if (!prop) {
                pr_err("Error %s getting device-name\n", DRIVER_NAME);
		return -EINVAL;
	}
        init_data->device_name = (char *)prop;
        driver_devel("%s: device-name=%s\n", DRIVER_NAME, init_data->device_name);
	return 0;
}

static int portal_of_probe(struct platform_device *pdev)
{
	struct portal_init_data init_data;
	int rc;

        driver_devel("portal_of_probe\n");

	memset(&init_data, 0, sizeof(struct portal_init_data));

	init_data.pdev = pdev;

	rc = portal_parse_hw_info(pdev->dev.of_node, &init_data);
        driver_devel("portal_parse_hw_info returned %d\n", rc);
	if (rc)
		return rc;

	return portal_init_driver(&init_data);
}

static int portal_of_remove(struct platform_device *pdev)
{
	return portal_deinit_driver(pdev);
}

static struct of_device_id portal_of_match[] __devinitdata = {
	{ .compatible = "linux,ushw-bridge-0.01.a" }, /* old name */
	{ .compatible = "linux,portal-0.01.a" },
	{/* end of table */},
};
MODULE_DEVICE_TABLE(of, portal_of_match);


static struct platform_driver portal_of_driver = {
	.probe = portal_of_probe,
	.remove = portal_of_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = DRIVER_NAME,
		.of_match_table = portal_of_match,
	},
};


static int __init portal_of_init(void)
{
	if (platform_driver_register(&portal_of_driver)) {
		pr_err("Error portal driver registration\n");
		return -ENODEV;
	}
	return 0;
}

static void __exit portal_of_exit(void)
{
        portal_ion_release();
	platform_driver_unregister(&portal_of_driver);
}


#ifndef MODULE
late_initcall(portal_of_init);
#else
module_init(portal_of_init);
module_exit(portal_of_exit);
#endif

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
