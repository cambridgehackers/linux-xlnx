/*
 * Xylon logiCVC frame buffer driver internal data structures
 *
 * Author: Xylon d.o.o.
 * e-mail: davor.joja@logicbricks.com
 *
 * 2012 (c) Xylon d.o.o.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#ifndef __XYLON_BB_DATA_H__
#define __XYLON_BB_DATA_H__


#include <linux/ion.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include "logicvc.h"


#define DRIVER_NAME "xylonbb"
#define DEVICE_NAME "logibitblit"
#define DRIVER_DESCRIPTION "Xylon logiBITBLIT"
#define DRIVER_VERSION "0.1"

/* BB driver flags */
#define BB_DMA_BUFFER        0x01
#define BB_MEMORY_LE         0x02
#define BB_VMODE_INIT        0x10
#define BB_DEFAULT_VMODE_SET 0x20
#define BB_VMODE_SET         0x40
#define BB_RESERVED_0x100    0x100


#ifdef DEBUG
#define driver_devel(format, ...) \
	do { \
		printk(KERN_DEBUG format, ## __VA_ARGS__); \
	} while (0)
#else
#define driver_devel(format, ...)
#endif

#define BB_NAME_SZ 20

struct xylonbb_registers {
	u32 dtype_reg;
};

struct xylonbb_register_access {
	u32 (*xylonbb_get_reg_val)
		(void *reg_base_virt, unsigned long offset);
	void (*xylonbb_set_reg_val)
		(u32 value, void *reg_base_virt, unsigned long offset);
};

struct xylonbb_common_data {
	struct device *dev;
        struct miscdevice misc;
        struct ion_device *ion_device;
        struct ion_client *ion_client;
	struct mutex irq_mutex;
        dma_addr_t reg_base_phys;
        void      *reg_base_virt;
	unsigned short xylonbb_flags;
	unsigned char xylonbb_irq;
	unsigned char xylonbb_use_ref;
};

struct xylonbb_init_data {
	struct platform_device *pdev;
	unsigned long vmem_base_addr;
	unsigned long vmem_high_addr;
	unsigned short flags;
	bool vmode_params_set;
};


/* xylonbb core interface functions */
extern int xylonbb_get_params(char *options);
extern int xylonbb_init_driver(struct xylonbb_init_data *init_data);
extern int xylonbb_deinit_driver(struct platform_device *pdev);

#endif /* __XYLON_BB_DATA_H__ */
