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

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/portal.h>

static void dump_regs(const char *prefix, struct portal_data *ushw_data)
{
        int i;
        for (i = 0; i < 10; i++) {
                unsigned long regval;
                regval = readl(ushw_data->reg_base_virt + i*4);
                driver_devel("%s reg %x value %08lx\n", prefix,
                             i*4, regval);
        }
}

static irqreturn_t portal_isr(int irq, void *dev_id)
{
	struct portal_data *ushw_data = (struct portal_data *)dev_id;
	u32 isr;


        isr = readl(ushw_data->reg_base_virt + 0);
	driver_devel("%s IRQ %d %x\n", __func__, irq, isr);
        // clear it
        if (!isr)
                isr = 1;
        ushw_data->int_status = isr;
        writel(isr, ushw_data->reg_base_virt + 0);

        dump_regs("ISR", ushw_data);
        mutex_unlock(&ushw_data->completion_mutex);
	wake_up_interruptible(&ushw_data->wait_queue);

        return IRQ_HANDLED;
}

static int portal_open(struct inode *inode, struct file *filep)
{
	struct miscdevice *miscdev = filep->private_data;
	struct portal_data *ushw_data =
                container_of(miscdev, struct portal_data, misc);

        int i;

        driver_devel("%s: %s\n", __FUNCTION__, ushw_data->device_name);
        for (i = 0; i < 8; i++) {
                unsigned long before;
                unsigned long after;
                before = readl(ushw_data->reg_base_virt + i*4);
                writel(0xdeadbeef + i, ushw_data->reg_base_virt + i*4);
                after = readl(ushw_data->reg_base_virt + i*4);
                driver_devel("%s reg %x before %08lx after %08lx\n", __func__,
                             i*4, before, after);
        }

        // clear status
        writel(0, ushw_data->reg_base_virt + 0);
        // enable interrupts
        writel(1, ushw_data->reg_base_virt + 4);
 #if 0
        if (ushw_data->timer_values[0]) {
                // start timer
                writel(0x0FFFFFF, ushw_data->reg_base_virt + ushw_data->timer_values[0]);
        }
        if (ushw_data->fifo_offset_req_resp[0]) {
                int i;
                u32 args[2] = { 0xfeed0000, 0x0000beef };
                for (i = 0; i < ushw_data->fifo_offset_req_resp[1] / 4; i++) {
                        writel(args[i], ushw_data->reg_base_virt + ushw_data->fifo_offset_req_resp[0]);
                }
        }
#endif
	return 0;
}

long portal_unlocked_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *miscdev = filep->private_data;
	struct portal_data *ushw_data =
                container_of(miscdev, struct portal_data, misc);

        switch (cmd) {
	case USHW_PUTGET:
	case USHW_PUT: {
                PortalMessage msg;
                unsigned int buf[128];
                int i;
		if (copy_from_user(&msg, (void __user *)arg, sizeof(msg)))
			return -EFAULT;
                printk("%s: copying message body\n", __FUNCTION__);
		if (copy_from_user(&buf, (void __user *)arg+sizeof(msg), msg.size))
			return -EFAULT;
                printk("%s: writing args\n", __FUNCTION__);
                for (i = 0; i < ushw_data->fifo_offset_req_resp[1] / 4; i++) {
                        printk("arg %x %08x\n", i*4, buf[i]);
                        writel(buf[i], ushw_data->reg_base_virt + ushw_data->fifo_offset_req_resp[0]);
                }
                dump_regs("PUT", ushw_data);
                if (cmd == USHW_PUTGET) {
                        printk("%s: PUTGET acquiring completion_mutex\n", __FUNCTION__);
                        mutex_lock_interruptible(&ushw_data->completion_mutex);
                        for (i = 0; i < ushw_data->fifo_offset_req_resp[2] / 4; i++) {
                                printk("%s: result %x %x\n", __FUNCTION__, i*4, ushw_data->buf[i]);
                        }
                        if (msg.size)
                          if (copy_to_user((void __user *)arg+sizeof(msg),
                                           ushw_data->buf, ushw_data->fifo_offset_req_resp[2]))
                                        return -EFAULT;
                }
                return 0;
        } break;
	case USHW_GET: {
                PortalMessage msg;
                printk("%s: GET\n", __FUNCTION__);
		if (copy_from_user(&msg, (void __user *)arg, sizeof(msg)))
			return -EFAULT;

                dump_regs("GET", ushw_data);
                if (ushw_data->fifo_offset_req_resp[0]) {
                        int i;
                        for (i = 0; i < ushw_data->fifo_offset_req_resp[2]/4; i++) {
                                ushw_data->buf[i] = 
                                        readl(ushw_data->reg_base_virt
                                              + ushw_data->fifo_offset_req_resp[0]);
                                printk("%s: result %x %x\n", __FUNCTION__, i*4, ushw_data->buf[i]);
                        }
                }

                if (ushw_data->fifo_offset_req_resp[2])
                        if (copy_to_user((void __user *)arg+sizeof(msg)+msg.size,
                                         ushw_data->buf, sizeof(ushw_data->fifo_offset_req_resp[2])))
                                return -EFAULT;
                return 0;
        } break;
        default:
                printk("portal_unlocked_ioctl ENOTTY cmd=%x\n", cmd);
                return -ENOTTY;
        }

        return -ENODEV;
}

unsigned int portal_poll (struct file *filep, poll_table *poll_table)
{
	struct miscdevice *miscdev = filep->private_data;
	struct portal_data *ushw_data =
                container_of(miscdev, struct portal_data, misc);
        int int_status = readl(ushw_data->reg_base_virt + 0);
        int mask = 0;
        poll_wait(filep, &ushw_data->wait_queue, poll_table);
        if (int_status & 1)
                mask = POLLIN | POLLRDNORM;
        printk("%s: int_status=%x mask=%x\n", __FUNCTION__, int_status, mask);
        return mask;
}

static int portal_release(struct inode *inode, struct file *filep)
{
	driver_devel("%s inode=%p filep=%p\n", __func__, inode, filep);
        return 0;
}

static const struct file_operations portal_fops = {
	.open = portal_open,
        .unlocked_ioctl = portal_unlocked_ioctl,
        .poll = portal_poll,
	.release = portal_release,
};

int portal_init_driver(struct portal_init_data *init_data)
{
	struct device *dev;
	struct portal_data *ushw_data;
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
		pr_err("Error portal resources\n");
		return -ENODEV;
	}

	ushw_data = kzalloc(sizeof(struct portal_data), GFP_KERNEL);
	if (!ushw_data) {
		pr_err("Error portal allocating internal data\n");
		rc = -ENOMEM;
		goto err_mem;
	}
        ushw_data->device_name = init_data->device_name;
        memcpy(ushw_data->timer_values, init_data->timer_values, sizeof(init_data->timer_values));
        memcpy(ushw_data->fifo_offset_req_resp,
               init_data->fifo_offset_req_resp, sizeof(init_data->fifo_offset_req_resp));

	reg_base_phys = reg_res->start;
	reg_range = reg_res->end - reg_res->start;
	reg_base_virt = ioremap_nocache(reg_base_phys, reg_range);
        pr_info("%s reg_base phys %x/%x virt %p\n",
                ushw_data->device_name,
                reg_base_phys, reg_range, reg_base_virt);
        ushw_data->reg_base_phys = reg_base_phys;
        ushw_data->reg_base_virt = reg_base_virt;

        mutex_init(&ushw_data->reg_mutex);
        mutex_init(&ushw_data->completion_mutex);
        mutex_lock(&ushw_data->completion_mutex);
        init_waitqueue_head(&ushw_data->wait_queue);

	ushw_data->portal_irq = irq_res->start;
	if (request_irq(ushw_data->portal_irq, portal_isr,
			IRQF_TRIGGER_HIGH, ushw_data->device_name, ushw_data)) {
		ushw_data->portal_irq = 0;
		goto err_bb;
	}

	ushw_data->dev = dev;
	dev_set_drvdata(dev, (void *)ushw_data);

        miscdev = &ushw_data->misc;
        driver_devel("%s:%d miscdev=%p\n", __func__, __LINE__, miscdev);
        driver_devel("%s:%d ushw_data=%p\n", __func__, __LINE__, ushw_data);
        miscdev->minor = MISC_DYNAMIC_MINOR;
        miscdev->name = ushw_data->device_name;
        miscdev->fops = &portal_fops;
        miscdev->parent = NULL;
        misc_register(miscdev);

	return 0;

err_bb:
	if (ushw_data->portal_irq != 0)
		free_irq(ushw_data->portal_irq, ushw_data);

err_mem:
	if (ushw_data) {
		kfree(ushw_data);
	}

	dev_set_drvdata(dev, NULL);

	return rc;
}

int portal_deinit_driver(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct portal_data *ushw_data = 
                (struct portal_data *)dev_get_drvdata(dev);

	driver_devel("%s\n", __func__);

	if (ushw_data->portal_use_ref) {
		pr_err("Error portal in use\n");
		return -EINVAL;
	}

	free_irq(ushw_data->portal_irq, ushw_data);
	kfree(ushw_data);

	dev_set_drvdata(dev, NULL);

	return 0;
}

static int portal_parse_hw_info(struct device_node *np,
                                     struct portal_init_data *init_data)
{
	u32 const *prop;
	int size;
        int status = 0;

	prop = of_get_property(np, "device-name", &size);
	if (!prop) {
                pr_err("Error %s getting device-name\n", DRIVER_NAME);
		return -EINVAL;
	}
        init_data->device_name = (char *)prop;
        driver_devel("%s: device-name=%s\n", DRIVER_NAME, init_data->device_name);

	status = of_property_read_u32_array(np, "timer", init_data->timer_values,
                                            sizeof(init_data->timer_values)/sizeof(u32));
        if (!status)
                driver_devel("%s: timer=%x %d\n", DRIVER_NAME, init_data->timer_values[0], init_data->timer_values[1]);
	status = of_property_read_u32_array(np, "fifo", init_data->fifo_offset_req_resp,
                                            sizeof(init_data->fifo_offset_req_resp)/sizeof(u32));
        if (!status)
                driver_devel("%s: fifo=%x %d %d\n", DRIVER_NAME,
                             init_data->fifo_offset_req_resp[0],
                             init_data->fifo_offset_req_resp[1],
                             init_data->fifo_offset_req_resp[2]);
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
	{ .compatible = "linux,ushw-bridge-0.01.a" },
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
