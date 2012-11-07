/*
 * Xylon logiCVC frame buffer driver core functions
 *
 * Author: Xylon d.o.o.
 * e-mail: davor.joja@logicbricks.com
 *
 * This driver was based on skeletonfb.c and other framebuffer video drivers.
 * 2012 Xylon d.o.o.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

/*
	Usefull driver information:
	- driver does not support multiple instances of logiCVC-ML
	- logiCVC-ML background layer is recomended
	- platform driver default resolution is set with defines in xylonfb-vmode.h
 */


#include <linux/module.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/console.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include "xylonbb.h"

static char *xylonbb_mode_option;

/* Function declarations */
extern int xylonbb_hw_pixclk_set(unsigned long pixclk_khz);
extern bool xylonbb_hw_pixclk_change(void);


static u32 xylonbb_get_reg(void *base_virt, unsigned long offset)
{
	return readl(base_virt + offset);
}

static void xylonbb_set_reg(u32 value,
	void *base_virt, unsigned long offset)
{
	writel(value, (base_virt + offset));
}

static u32 xylonbb_get_reg_mem_addr(void *base_virt, unsigned long offset)
{
	unsigned long ordinal = offset >> 3;

        return (u32)base_virt+offset;
}

static u32 xylonbb_get_reg_mem(void *base_virt, unsigned long offset)
{
	return (*((u32 *)xylonbb_get_reg_mem_addr(base_virt, offset)));
}

static void xylonbb_set_reg_mem(u32 value,
	void *base_virt, unsigned long offset)
{
	u32 *reg_mem_addr =
		(u32 *)xylonbb_get_reg_mem_addr(base_virt, offset);
	*reg_mem_addr = value;
	writel((*reg_mem_addr), (base_virt + offset));
}


static irqreturn_t xylonbb_isr(int irq, void *dev_id)
{
	struct xylonbb_info **abbi = (struct xylonbb_info **)dev_id;
	struct xylonbb_info *bbi = abbi[0];
	struct xylonbb_common_data *common_data;
	u32 isr;

	driver_devel("%s IRQ %d\n", __func__, irq);

#if 0
	isr = readl(layer_data->reg_base_virt + LOGICVC_INT_STAT_ROFF);
	if (isr & LOGICVC_V_SYNC_INT) {
		writel(LOGICVC_V_SYNC_INT,
			layer_data->reg_base_virt + LOGICVC_INT_STAT_ROFF);
		common_data->xylonbb_vsync.cnt++;
		wake_up_interruptible(&common_data->xylonbb_vsync.wait);
		return IRQ_HANDLED;
	} else {
		return IRQ_NONE;
	}
#endif
		return IRQ_NONE;
}

static int xylonbb_open(struct xylonbb_info *bbi, int user)
{
	driver_devel("%s\n", __func__);

	return 0;
}

static int xylonbb_release(struct xylonbb_info *bbi, int user)
{
	driver_devel("%s\n", __func__);

	return 0;
}

static int xylonbb_register_bb(struct xylonbb_info *bbi,
                               u32 reg_base_phys, int id, int *regbb)
{
	struct xylonbb_common_data *common_data;
	int alpha;

	driver_devel("%s\n", __func__);

	bbi->flags = 0;

	sprintf(bbi->id, "Xylon BB%d", id);

        driver_devel("xylonbb needs registration\n");

	pr_info("xylonbb %d registered\n", id);
	/* after bb driver registration, values in struct xylonbb_info
		must not be changed anywhere else except in xylonbb_set_par */

	return 0;
}

static void xylonbb_start(struct xylonbb_info **abbi)
{
	int i;

	driver_devel("%s\n", __func__);
}

int xylonbb_init_driver(struct xylonbb_init_data *init_data)
{
	struct device *dev;
	struct xylonbb_info **abbi;
	struct xylonbb_info *bbi;
	struct xylonbb_common_data *common_data;
	struct resource *reg_res, *irq_res;
	void *reg_base_virt;
	u32 reg_base_phys;
	int reg_range;
	int i, rc;
	int regbb[1];
	driver_devel("%s\n", __func__);

	dev = &init_data->pdev->dev;

	reg_res = platform_get_resource(init_data->pdev, IORESOURCE_MEM, 0);
	irq_res = platform_get_resource(init_data->pdev, IORESOURCE_IRQ, 0);
	if ((!reg_res) || (!irq_res)) {
		pr_err("Error xylonbb resources\n");
		return -ENODEV;
	}

	abbi = kzalloc(sizeof(struct xylonbb_info *), GFP_KERNEL);
	common_data = kzalloc(sizeof(struct xylonbb_common_data), GFP_KERNEL);
	if (!abbi || !common_data) {
		pr_err("Error xylonbb allocating internal data\n");
		rc = -ENOMEM;
		goto err_mem;
	}

	common_data->xylonbb_flags |= BB_VMODE_INIT;
	if (init_data->flags & LOGICVC_READABLE_REGS) {
		common_data->xylonbb_flags |= LOGICVC_READABLE_REGS;
		common_data->reg_access.xylonbb_get_reg_val = xylonbb_get_reg;
		common_data->reg_access.xylonbb_set_reg_val = xylonbb_set_reg;
	} else {
		common_data->reg_list =
			kzalloc(sizeof(struct xylonbb_registers), GFP_KERNEL);
		common_data->reg_access.xylonbb_get_reg_val = xylonbb_get_reg_mem;
		common_data->reg_access.xylonbb_set_reg_val = xylonbb_set_reg_mem;
	}

	reg_base_phys = reg_res->start;
	reg_range = reg_res->end - reg_res->start;
	reg_base_virt = ioremap_nocache(reg_base_phys, reg_range);

	common_data->xylonbb_irq = irq_res->start;
	if (request_irq(common_data->xylonbb_irq, xylonbb_isr,
			IRQF_TRIGGER_HIGH, DEVICE_NAME, abbi)) {
		common_data->xylonbb_irq = 0;
		goto err_bb;
	}

#if defined(__LITTLE_ENDIAN)
	common_data->xylonbb_flags |= BB_MEMORY_LE;
#endif
#if 0
	mutex_init(&common_data->irq_mutex);
	init_waitqueue_head(&common_data->xylonbb_vsync.wait);
	common_data->xylonbb_use_ref = 0;

	common_data->xylonbb_flags &=
		~(BB_VMODE_INIT | BB_DEFAULT_VMODE_SET | BB_VMODE_SET);
	xylonbb_mode_option = NULL;

	common_data->dev = dev;
#endif
	dev_set_drvdata(dev, (void *)abbi);

	/* start HW */
	xylonbb_start(abbi);

	return 0;

err_bb:
	if (common_data->xylonbb_irq != 0)
		free_irq(common_data->xylonbb_irq, abbi);

err_mem:
	if (common_data) {
		if (common_data->reg_list)
			kfree(common_data->reg_list);
		kfree(common_data);
	}
	if (abbi)
		kfree(abbi);

	dev_set_drvdata(dev, NULL);

	return rc;
}

int xylonbb_deinit_driver(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xylonbb_info **abbi = (struct xylonbb_info **)dev_get_drvdata(dev);
	struct xylonbb_info *bbi = abbi[0];
	struct xylonbb_common_data *common_data;
	int i;
	bool logicvc_unmap;

	driver_devel("%s\n", __func__);

	if (common_data->xylonbb_use_ref) {
		pr_err("Error xylonbb in use\n");
		return -EINVAL;
	}

	logicvc_unmap = false;

	free_irq(common_data->xylonbb_irq, abbi);
	kfree(common_data);
	kfree(abbi);

	dev_set_drvdata(dev, NULL);

	return 0;
}

#ifndef MODULE
int xylonbb_get_params(char *options)
{
	char *this_opt;

	driver_devel("%s\n", __func__);

	if (!options || !*options)
		return 0;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;
		xylonbb_mode_option = this_opt;
	}
	return 0;
}
#endif
