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

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/dma-buf.h>
#include <linux/portal.h>

#include <linux/slab.h>
#include <linux/scatterlist.h>
#include "../../gpu/ion/ion_priv.h"

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

static void dump_regs(const char *prefix, struct portal_data *portal_data)
{
        int i;
        for (i = 0; i < 20; i++) {
                unsigned long regval;
                regval = readl(portal_data->reg_base_virt + i*4);
                driver_devel("%s reg %x value %08lx\n", prefix,
                             i*4, regval);
        }
}

static irqreturn_t portal_isr(int irq, void *dev_id)
{
	struct portal_data *portal_data = (struct portal_data *)dev_id;
	u32 isr;


        isr = readl(portal_data->reg_base_virt + 0);
	driver_devel("%s IRQ %d %x\n", __func__, irq, isr);
        // clear it
        if (!isr)
                isr = 1;
        portal_data->int_status = isr;
        writel(isr, portal_data->reg_base_virt + 0);

        dump_regs("ISR", portal_data);
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

        driver_devel("%s: %s ctrl %lx fifo %lx\n", __FUNCTION__, portal_data->device_name,
                     (long)portal_data->reg_base_phys,
                     (long)(portal_data->reg_base_phys + portal_data->fifo_offset_req_resp[0]));
        dump_regs("portal_open", portal_data);

        portal_client->ion_client = ion_client_create(portal_ion_device, 0xf, "portal_ion_client");
        portal_client->portal_data = portal_data;
        printk("portal created ion_client %p\n", portal_client->ion_client);
        filep->private_data = portal_client;

        if (portal_client->ion_client) {
                int i;
                struct ion_client *ion_client = portal_client->ion_client;
                struct ion_handle *handle = ion_alloc(ion_client, 4096, 0, 0xf, 0);
                printk("allocated handle %p\n", handle);
                int fd = ion_share_dma_buf(ion_client, handle);
                printk("sharing dma_buf fd %d\n", fd);
                void *mapping = ion_map_kernel(ion_client, handle);
                printk("mapped handle %p\n", mapping);
                struct dma_buf *dma_buf = dma_buf_get(fd);
                printk("dma_buf %p\n", dma_buf);
                struct dma_buf_attachment *attachment = dma_buf_attach(dma_buf, miscdev->this_device);
                struct sg_table *sg_table = dma_buf_map_attachment(attachment, DMA_TO_DEVICE);
                printk("sg_table %p nents %d\n", sg_table, sg_table->nents);
                for (i = 0; i < sg_table->nents; i++) {
                        printk("entry %d dma_address %x\n", i, sg_table->sgl[i].dma_address);
                }
                dma_buf_detach(dma_buf, attachment);
        }

        // clear status
        writel(0, portal_data->reg_base_virt + 0);
        // enable interrupts
        writel(1, portal_data->reg_base_virt + 4);
	return 0;
}

struct ion_handle {
	struct kref ref;
	struct ion_client *client;
	struct ion_buffer *buffer;
	struct rb_node node;
	unsigned int kmap_cnt;
};

long portal_unlocked_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;

        switch (cmd) {
        case PORTAL_ALLOC: {
                struct PortalAlloc alloc;
                struct dma_buf *dma_buf = 0;
                struct dma_buf_attachment *attachment = 0;
                struct sg_table *sg_table = 0;
                struct scatterlist *sg;
                int i;

		if (copy_from_user(&alloc, (void __user *)arg, sizeof(alloc)))
			return -EFAULT;
                alloc.size = round_up(alloc.size, 4096);
                struct ion_handle *handle = ion_alloc(portal_client->ion_client, alloc.size, 4096,
                                                      0xf, 0);
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
	case PORTAL_PUTGET:
	case PORTAL_PUT: {
                PortalMessage msg;
                unsigned int buf[128];
                int i;
		if (copy_from_user(&msg, (void __user *)arg, sizeof(msg)))
			return -EFAULT;
                if (0) printk("%s: copying message body\n", __FUNCTION__);
		if (copy_from_user(&buf, (void __user *)arg+sizeof(msg), msg.size))
			return -EFAULT;
                if (0) printk("%s: writing args at address %lx\n",
                              __FUNCTION__,
                              (long)(portal_data->reg_base_phys + portal_data->fifo_offset_req_resp[0]));
                mutex_lock(&portal_data->reg_mutex);
                for (i = 0; i < portal_data->fifo_offset_req_resp[1] / 4; i++) {
                        printk("arg %x %08x\n", i*4, buf[i]);
                        writel(buf[i], portal_data->reg_base_virt + portal_data->fifo_offset_req_resp[0]);
                }
                mutex_unlock(&portal_data->reg_mutex);
                //dump_regs("PUT", portal_data);
                if (cmd == PORTAL_PUTGET) {
                        printk("%s: PUTGET acquiring completion_mutex\n", __FUNCTION__);
                        mutex_lock_interruptible(&portal_data->completion_mutex);
                        if (0)
                        for (i = 0; i < portal_data->fifo_offset_req_resp[2] / 4; i++) {
                                printk("%s: result %x %08x\n", __FUNCTION__, i*4, portal_data->buf[i]);
                        }
                        if (msg.size)
                          if (copy_to_user((void __user *)arg+sizeof(msg),
                                           portal_data->buf, portal_data->fifo_offset_req_resp[2]))
                                        return -EFAULT;
                }
                return 0;
        } break;
	case PORTAL_GET: {
                PortalMessage msg;
                int int_status = readl(portal_data->reg_base_virt + 0);
                int mask = 0;
                printk("%s: GET int_status=%x mask=%x\n", __FUNCTION__, int_status, mask);
                if (int_status & 1 == 0)
                        return -EAGAIN;
		if (copy_from_user(&msg, (void __user *)arg, sizeof(msg)))
			return -EFAULT;

                //dump_regs("GET", portal_data);
                mutex_lock(&portal_data->reg_mutex);
                if (portal_data->fifo_offset_req_resp[0]) {
                        int i;
                        for (i = 0; i < portal_data->fifo_offset_req_resp[2]/4; i++) {
                                portal_data->buf[i] = 
                                        readl(portal_data->reg_base_virt
                                              + portal_data->fifo_offset_req_resp[0]);
                                //printk("%s: result %x %08x\n", __FUNCTION__, i*4, portal_data->buf[i]);
                        }
                }
                mutex_unlock(&portal_data->reg_mutex);

                if (copy_to_user((void __user *)arg,
                                 &portal_data->fifo_offset_req_resp[2],
                                 sizeof(portal_data->fifo_offset_req_resp[2])))
                        return -EFAULT;
                if (portal_data->fifo_offset_req_resp[2])
                        if (copy_to_user((void __user *)arg+sizeof(msg),
                                         portal_data->buf, portal_data->fifo_offset_req_resp[2]))
                                return -EFAULT;
                return 0;
        } break;
        default:
                printk("portal_unlocked_ioctl ENOTTY cmd=%x\n", cmd);
                return -ENOTTY;
        }

        return -ENODEV;
}

/* maps the region containing the fifos */
int portal_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;
	unsigned long off = portal_data->reg_base_phys + portal_data->fifo_offset_req_resp[0];
	u32 len = 1 << PAGE_SHIFT;
        if (!portal_client)
                return -ENODEV;
        if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
                return -EINVAL;
        if ((vma->vm_end - vma->vm_start + (vma->vm_pgoff << PAGE_SHIFT)) > len)
		return -EINVAL;
	vma->vm_pgoff = off >> PAGE_SHIFT;
	vma->vm_flags |= VM_IO | VM_RESERVED;
        if (io_remap_pfn_range(vma, vma->vm_start, off >> PAGE_SHIFT,
                               vma->vm_end - vma->vm_start, vma->vm_page_prot))
                return -EAGAIN;
        return 0;
}


unsigned int portal_poll (struct file *filep, poll_table *poll_table)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;
        int int_status = readl(portal_data->reg_base_virt + 0);
        int mask = 0;
        poll_wait(filep, &portal_data->wait_queue, poll_table);
        if (int_status & 1)
                mask = POLLIN | POLLRDNORM;
        //printk("%s: int_status=%x mask=%x\n", __FUNCTION__, int_status, mask);
        return mask;
}

static int portal_release(struct inode *inode, struct file *filep)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;
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
	void *reg_base_virt;
	u32 reg_base_phys;
	int reg_range;
	int rc;
	driver_devel("%s\n", __func__);

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
        memcpy(portal_data->timer_values, init_data->timer_values, sizeof(init_data->timer_values));
        memcpy(portal_data->fifo_offset_req_resp,
               init_data->fifo_offset_req_resp, sizeof(init_data->fifo_offset_req_resp));

	reg_base_phys = reg_res->start;
	reg_range = reg_res->end - reg_res->start;
	reg_base_virt = ioremap_nocache(reg_base_phys, reg_range);
        pr_info("%s reg_base phys %x/%x virt %p\n",
                portal_data->device_name,
                reg_base_phys, reg_range, reg_base_virt);
        portal_data->reg_base_phys = reg_base_phys;
        portal_data->reg_base_virt = reg_base_virt;

        mutex_init(&portal_data->reg_mutex);
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

	if (portal_data->portal_use_ref) {
		pr_err("Error portal in use\n");
		return -EINVAL;
	}

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
