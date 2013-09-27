/*
 * Generic userspace hardware bridge
 *
 * Author: Jamey Hicks <jamey.hicks@gmail.com>
 *
 * 2012 (c) Jamey Hicks
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#ifndef __PORTAL_H__
#define __PORTAL_H__


#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/wait.h>


/////////////////////////////////////////////////////////////
// copied from ion_priv.h

/**
 * enum ion_heap_types - list of all possible types of heaps
 * @ION_HEAP_TYPE_SYSTEM:	 memory allocated via vmalloc
 * @ION_HEAP_TYPE_SYSTEM_CONTIG: memory allocated via kmalloc
 * @ION_HEAP_TYPE_CARVEOUT:	 memory allocated from a prereserved
 * 				 carveout heap, allocations are physically
 * 				 contiguous
 * @ION_NUM_HEAPS:		 helper for iterating over heaps, a bit mask
 * 				 is used to identify the heaps, so only 32
 * 				 total heap types are supported
 */
enum ion_heap_type {
	ION_HEAP_TYPE_SYSTEM,
	ION_HEAP_TYPE_SYSTEM_CONTIG,
	ION_HEAP_TYPE_CARVEOUT,
	ION_HEAP_TYPE_CUSTOM, /* must be last so device specific heaps always
				 are at the end of this enum */
	ION_NUM_HEAPS = 16,
};

#define ION_HEAP_SYSTEM_MASK		(1 << ION_HEAP_TYPE_SYSTEM)
#define ION_HEAP_SYSTEM_CONTIG_MASK	(1 << ION_HEAP_TYPE_SYSTEM_CONTIG)
#define ION_HEAP_CARVEOUT_MASK		(1 << ION_HEAP_TYPE_CARVEOUT)

/**
 * heap flags - the lower 16 bits are used by core ion, the upper 16
 * bits are reserved for use by the heaps themselves.
 */
#define ION_FLAG_CACHED 1		/* mappings of this buffer should be
					   cached, ion will do cache
					   maintenance when the buffer is
					   mapped for dma */
#define ION_FLAG_CACHED_NEEDS_SYNC 2	/* mappings of this buffer will created
					   at mmap time, if this is set
					   caches must be managed manually */

struct ion_device;
struct ion_heap;
struct ion_mapper;
struct ion_client;
struct ion_buffer;

/* This should be removed some day when phys_addr_t's are fully
   plumbed in the kernel, and all instances of ion_phys_addr_t should
   be converted to phys_addr_t.  For the time being many kernel interfaces
   do not accept phys_addr_t's that would have to */
#define ion_phys_addr_t unsigned long

/**
 * struct ion_platform_heap - defines a heap in the given platform
 * @type:	type of the heap from ion_heap_type enum
 * @id:		unique identifier for heap.  When allocating (lower numbers 
 * 		will be allocated from first)
 * @name:	used for debug purposes
 * @base:	base address of heap in physical memory if applicable
 * @size:	size of the heap in bytes if applicable
 *
 * Provided by the board file.
 */
struct ion_platform_heap {
	enum ion_heap_type type;
	unsigned int id;
	const char *name;
	ion_phys_addr_t base;
	size_t size;
};

// 
/////////////////////////////////////////////////////////////



#define DRIVER_NAME "portal"
#define DRIVER_DESCRIPTION "Generic userspace hardware bridge"
#define DRIVER_VERSION "0.1"

#ifdef DEBUG // was KERN_DEBUG
#define driver_devel(format, ...) \
	do { \
		printk(format, ## __VA_ARGS__); \
	} while (0)
#else
#define driver_devel(format, ...)
#endif

#define PORTAL_NAME_SZ 20

struct portal_data;
struct ion_client;

struct portal_client {
    struct portal_data *portal_data;
    struct ion_client *ion_client;
};

struct portal_data {
	struct device *dev;
        struct miscdevice misc;
        struct ion_device *ion_device;
        struct mutex completion_mutex;
        wait_queue_head_t wait_queue;
        const char *device_name;
        dma_addr_t dev_base_phys;
        dma_addr_t ind_reg_base_phys;
        dma_addr_t ind_fifo_base_phys;
        dma_addr_t req_reg_base_phys;
        dma_addr_t req_fifo_base_phys;
        void      *dev_base_virt;
        void      *ind_reg_base_virt;
        void      *ind_fifo_base_virt;
        void      *req_reg_base_virt;
        void      *req_fifo_base_virt;
	unsigned char portal_irq;
};

struct portal_init_data {
	struct platform_device *pdev;
        const char *device_name;
};


extern int portal_init_driver(struct portal_init_data *init_data);
extern int portal_deinit_driver(struct platform_device *pdev);

#endif /* __PORTAL_H__ */
