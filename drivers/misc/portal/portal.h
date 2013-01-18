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


#include <linux/ion.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/wait.h>

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
	struct mutex reg_mutex;
        struct mutex completion_mutex;
        wait_queue_head_t wait_queue;
        const char *device_name;
        dma_addr_t reg_base_phys;
        void      *reg_base_virt;
	unsigned short portal_flags;
	unsigned char portal_irq;
	unsigned char portal_use_ref;
        unsigned long int_status;

        u32 timer_values[2];
        u32 fifo_offset_req_resp[3];
        u32 buf[128];
};

struct portal_init_data {
	struct platform_device *pdev;
	unsigned long vmem_base_addr;
	unsigned long vmem_high_addr;
        const char *device_name;
        u32 timer_values[2];
        u32 fifo_offset_req_resp[3];
	unsigned short flags;
	bool vmode_params_set;
};


extern int portal_init_driver(struct portal_init_data *init_data);
extern int portal_deinit_driver(struct platform_device *pdev);

#endif /* __PORTAL_H__ */
