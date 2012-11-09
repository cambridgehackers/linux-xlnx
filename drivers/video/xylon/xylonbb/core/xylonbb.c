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
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/ion.h>
#include <linux/uaccess.h>
#include <linux/console.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "xylonbb.h"
#include <linux/xylonbb.h>

static char *xylonbb_mode_option;

#define LOGIBITBLIT_CTRL0_ROFF 0x0000
#define LOGIBITBLIT_CTRL0_SRCLIN (1 << 0)
#define LOGIBITBLIT_CTRL0_DSTLIN (1 << 1)
#define LOGIBITBLIT_CTRL0_START  (1 << 7)
#define LOGIBITBLIT_CTRL0_BUSY   (1 << 7)
#define LOGIBITBLIT_CTRL0_RESET  (1 << 8)

#define LOGIBITBLIT_CTRL1_ROFF 0x0004
#define LOGIBITBLIT_CTRL1_COLFMT_ARGB8888 6

#define LOGIBITBLIT_ROP_ROFF   0x0008
#define LOGIBITBLIT_ROP_PD_S_OVER_D 0x0
#define LOGIBITBLIT_ROP_PD_FLOOR 0x10

#define LOGIBITBLIT_OP_ROFF  0x000C
#define LOGIBITBLIT_OP_PATTERN_FILL 0x7
#define LOGIBITBLIT_OP_PORTER_DUFF  0x8

#define LOGIBITBLIT_SRC_ADDR_ROFF   0x0010
#define LOGIBITBLIT_DST_ADDR_ROFF   0x0014
#define LOGIBITBLIT_SRC_STRIPE_ROFF 0x0018
#define LOGIBITBLIT_DST_STRIPE_ROFF 0x001C
#define LOGIBITBLIT_XWIDTH_ROFF     0x0020
#define LOGIBITBLIT_YWIDTH_ROFF     0x0024
#define LOGIBITBLIT_BG_COL_ROFF     0x0028
#define LOGIBITBLIT_FG_COL_ROFF     0x002C
#define LOGIBITBLIT_GLB_ALPHA_ROFF  0x0030
#define LOGIBITBLIT_IP_VERSION_ROFF 0x0034
#define LOGIBITBLIT_INT_STATUS_ROFF 0x0038
#define LOGIBITBLIT_INT_ENABLE_ROFF 0x003C

static irqreturn_t xylonbb_isr(int irq, void *dev_id)
{
	struct xylonbb_common_data *common_data = (struct xylonbb_common_data *)dev_id;
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

static int xylonbb_open(struct inode *inode, struct file *filep)
{
	struct miscdevice *miscdev = filep->private_data;
	struct xylonbb_common_data *common_data =
                container_of(miscdev, struct xylonbb_common_data, misc);

        driver_devel("%s:%d miscdev=%p\n", __func__, __LINE__, miscdev);
        driver_devel("%s:%d common_data=%p\n", __func__, __LINE__, common_data);
        driver_devel("%s:%d reg_base_virt=%p\n", __func__, __LINE__, common_data->reg_base_virt);
        driver_devel("%s ip_version %08x\n", __func__,
                     readl(common_data->reg_base_virt + LOGIBITBLIT_IP_VERSION_ROFF));
        driver_devel("%s int_status %08x\n", __func__,
                     readl(common_data->reg_base_virt + LOGIBITBLIT_INT_STATUS_ROFF));
        driver_devel("%s int_enable %08x\n", __func__,
                     readl(common_data->reg_base_virt + LOGIBITBLIT_INT_ENABLE_ROFF));
	return 0;
}

long xylonbb_unlocked_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *miscdev = filep->private_data;
	struct xylonbb_common_data *common_data =
                container_of(miscdev, struct xylonbb_common_data, misc);

        driver_devel("xylonbb_ioctl cmd=%x arg=%lx ctrl0=%x status=%x\n",
               cmd, arg,
               readl(common_data->reg_base_virt + LOGIBITBLIT_CTRL0_ROFF),
               readl(common_data->reg_base_virt + LOGIBITBLIT_INT_STATUS_ROFF));
        switch (cmd) {
	case XYLONBB_IOC_BITBLIT: {
		struct xylonbb_params params;
                ion_phys_addr_t src_dma_addr, dst_dma_addr;
                struct ion_handle *src_ion_handle, *dst_ion_handle;
                size_t src_dma_len, dst_dma_len;
                int status;
                struct ion_client *ion_client = common_data->ion_client;
                if (!ion_client)
                        return -ENODEV;
                
		if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
			return -EFAULT;

                src_ion_handle = ion_import_dma_buf(ion_client, params.src_dma_buf);
                driver_devel("%s:%d src_dma_buf=%d src_ion_handle=%p\n",
                       __func__, __LINE__, params.src_dma_buf, src_ion_handle);
                dst_ion_handle = ion_import_dma_buf(ion_client, params.dst_dma_buf);
                driver_devel("%s:%d dst_dma_buf=%d dst_ion_handle=%p\n",
                       __func__, __LINE__, params.dst_dma_buf, dst_ion_handle);
                status = ion_phys(ion_client, src_ion_handle, &src_dma_addr, &src_dma_len);
                driver_devel("%s:%d status=%d src_dma_addr=%0lx\n",
                       __func__, __LINE__, status, src_dma_addr);
                if (!src_dma_addr)
                        return status;

                status = ion_phys(ion_client, dst_ion_handle, &dst_dma_addr, &dst_dma_len);
                driver_devel("%s:%d status=%d dst_dma_addr=%0lx\n",
                       __func__, __LINE__, status, dst_dma_addr);
                if (!dst_dma_addr)
                        return status;

                writel(LOGIBITBLIT_ROP_PD_S_OVER_D, 
                       common_data->reg_base_virt + LOGIBITBLIT_ROP_ROFF);
                writel(LOGIBITBLIT_OP_PORTER_DUFF, 
                       common_data->reg_base_virt + LOGIBITBLIT_OP_ROFF);

                writel(src_dma_addr, common_data->reg_base_virt + LOGIBITBLIT_SRC_ADDR_ROFF);
                writel(params.src_stripe, common_data->reg_base_virt + LOGIBITBLIT_SRC_STRIPE_ROFF);
                writel(dst_dma_addr, common_data->reg_base_virt + LOGIBITBLIT_DST_ADDR_ROFF);
                writel(params.dst_stripe, common_data->reg_base_virt + LOGIBITBLIT_DST_STRIPE_ROFF);
                writel(params.num_columns, common_data->reg_base_virt + LOGIBITBLIT_XWIDTH_ROFF);
                writel(params.num_rows, common_data->reg_base_virt + LOGIBITBLIT_YWIDTH_ROFF);
                
                writel(LOGIBITBLIT_CTRL1_COLFMT_ARGB8888,
                       common_data->reg_base_virt + LOGIBITBLIT_CTRL1_ROFF);
                if (ion_client)
                        writel(LOGIBITBLIT_CTRL0_SRCLIN
                               | LOGIBITBLIT_CTRL0_DSTLIN
                               | LOGIBITBLIT_CTRL0_START,
                               common_data->reg_base_virt + LOGIBITBLIT_CTRL0_ROFF);

                // wait for completion
                do {
                        status = readl(common_data->reg_base_virt + LOGIBITBLIT_INT_STATUS_ROFF);
                        driver_devel("%s:%d status=%x\n", __func__, __LINE__, status);
                } while (status == 0);
                writel(7, common_data->reg_base_virt + LOGIBITBLIT_INT_STATUS_ROFF);

                ion_free(ion_client, src_ion_handle);
                ion_free(ion_client, dst_ion_handle);

                return 0;
        } break;
        default:
                return -ENOTTY;
        }

        return -ENODEV;
}

static void xylonbb_release(struct inode *inode)
{
	driver_devel("%s\n", __func__);

	return 0;
}

static const struct file_operations xylonbb_fops = {
	.open = xylonbb_open,
        .unlocked_ioctl = xylonbb_unlocked_ioctl,
	.release = xylonbb_release,
};

extern struct ion_device *xylon_ion_device;

int xylonbb_init_driver(struct xylonbb_init_data *init_data)
{
	struct device *dev;
	struct xylonbb_common_data *common_data;
	struct resource *reg_res, *irq_res;
        struct miscdevice *miscdev;
	void *reg_base_virt;
	u32 reg_base_phys;
	int reg_range;
	int rc;
	driver_devel("%s\n", __func__);

	dev = &init_data->pdev->dev;

	reg_res = platform_get_resource(init_data->pdev, IORESOURCE_MEM, 0);
	irq_res = platform_get_resource(init_data->pdev, IORESOURCE_IRQ, 0);
	if ((!reg_res) || (!irq_res)) {
		pr_err("Error xylonbb resources\n");
		return -ENODEV;
	}

	common_data = kzalloc(sizeof(struct xylonbb_common_data), GFP_KERNEL);
	if (!common_data) {
		pr_err("Error xylonbb allocating internal data\n");
		rc = -ENOMEM;
		goto err_mem;
	}

	reg_base_phys = reg_res->start;
	reg_range = reg_res->end - reg_res->start;
	reg_base_virt = ioremap_nocache(reg_base_phys, reg_range);
        pr_info("logiBITBLIT reg_base phys %x/%x virt %p\n",
                reg_base_phys, reg_range, reg_base_virt);
        common_data->reg_base_phys = reg_base_phys;
        common_data->reg_base_virt = reg_base_virt;

	common_data->xylonbb_irq = irq_res->start;
	if (request_irq(common_data->xylonbb_irq, xylonbb_isr,
			IRQF_TRIGGER_HIGH, DEVICE_NAME, common_data)) {
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

#endif
	common_data->dev = dev;
	dev_set_drvdata(dev, (void *)common_data);

        common_data->ion_client = ion_client_create(xylon_ion_device, 0xF, "xylonbb");
        driver_devel("%s:%d ion_client=%p\n", __func__, __LINE__, common_data->ion_client);

        miscdev = &common_data->misc;
        driver_devel("%s:%d miscdev=%p\n", __func__, __LINE__, miscdev);
        driver_devel("%s:%d common_data=%p\n", __func__, __LINE__, common_data);
        miscdev->minor = MISC_DYNAMIC_MINOR;
        miscdev->name = "xylonbb";
        miscdev->fops = &xylonbb_fops;
        miscdev->parent = NULL;
        misc_register(miscdev);

	return 0;

err_bb:
	if (common_data->xylonbb_irq != 0)
		free_irq(common_data->xylonbb_irq, common_data);

err_mem:
	if (common_data) {
		kfree(common_data);
	}

	dev_set_drvdata(dev, NULL);

	return rc;
}

int xylonbb_deinit_driver(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xylonbb_common_data *common_data = 
                (struct xylonbb_common_data *)dev_get_drvdata(dev);

	driver_devel("%s\n", __func__);

	if (common_data->xylonbb_use_ref) {
		pr_err("Error xylonbb in use\n");
		return -EINVAL;
	}

	free_irq(common_data->xylonbb_irq, common_data);
	kfree(common_data);

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
