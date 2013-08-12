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

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/dma-buf.h>
#include <linux/portal.h>

#include <linux/slab.h>
#include <linux/scatterlist.h>
#include "../../gpu/ion/ion_priv.h"

#include <asm/cacheflush.h>

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
	u32 int_src, int_en;


        //dump_regs("ISR a", portal_data);

        int_src = readl(portal_data->reg_base_virt + 0);
	int_en  = readl(portal_data->reg_base_virt + 4);
	//driver_devel("%s IRQ %d %x %x\n", __func__, irq, int_src, int_en);

        portal_data->int_status = 1;

	// disable interrupt.  this will be enabled by user mode 
	// driver  after all the HW->SW FIFOs have been emptied
        writel(0, portal_data->reg_base_virt + 0x4);

        //dump_regs("ISR b", portal_data);
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

        portal_data->fifo_base_offset = readl(portal_data->reg_base_virt + 0x00C);
        if (!portal_data->fifo_base_offset)
                portal_data->fifo_base_offset = 0x10000; // default

        driver_devel("%s: %s ctrl %lx fifo %lx\n", __FUNCTION__, portal_data->device_name,
                     (long)portal_data->reg_base_phys, (long)(portal_data->fifo_base_phys));
        // dump_regs("portal_open", portal_data);

        portal_client->ion_client = ion_client_create(portal_ion_device, 0xf, "portal_ion_client");
        portal_client->portal_data = portal_data;
        printk("portal created ion_client %p\n", portal_client->ion_client);
        filep->private_data = portal_client;

        // clear status (ignored by HW)
        writel(0, portal_data->reg_base_virt + 0);
        // enable interrupts
        writel(1, portal_data->reg_base_virt + 4);

	// sanity check, see if interrupts have been enabled
        // dump_regs("enable interrupts", portal_data);

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
	case PORTAL_DCACHE_FLUSH_INVAL: {
	  struct PortalAlloc alloc;
	  int i;
	  if (copy_from_user(&alloc, (void __user *)arg, sizeof(alloc)))
	    return -EFAULT;
	  //printk("portal_dcache_flush_inval\n");
	  for(i = 0; i < alloc.numEntries; i++){
	    char* start_addr = alloc.entries[i].dma_address;
	    unsigned int length = alloc.entries[i].length;
	    char* end_addr = start_addr + length;
	    //printk("portal_dcache_flush_inval[%d] %08x %d\n", i, start_addr, length);
	    // we saw this funciton invoked in arch/arm/mm/dma-mapping.c it works on physical addresses.
	    outer_clean_range(alloc.entries[i].dma_address, alloc.entries[i].dma_address+alloc.entries[i].length);
	    outer_inv_range(alloc.entries[i].dma_address, alloc.entries[i].dma_address+alloc.entries[i].length);
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
	case PORTAL_PUT: {
                PortalMessage msg;
                unsigned int buf[128];
                int i;
                long fifo_phys;
		if (copy_from_user(&msg, (void __user *)arg, sizeof(msg)))
			return -EFAULT;
                fifo_phys = (long)(portal_data->fifo_base_phys + msg.channel * 256);
                if (0)
                printk("%s: size=%d channel=%d fifoaddr=%lx\n", __FUNCTION__,
                       msg.size, msg.channel, fifo_phys);
                if (0) printk("%s: copying message body\n", __FUNCTION__);
		if (copy_from_user(&buf, (void __user *)arg+sizeof(msg), msg.size))
			return -EFAULT;
                if (0) printk("%s: writing args at address %lx\n",
                              __FUNCTION__,
                              (long)(portal_data->fifo_base_phys));
                mutex_lock(&portal_data->reg_mutex);
                for (i = 0; i < msg.size / 4; i++) {
                  //printk("arg %x %08x\n", i*4, buf[i]);
                        writel(buf[i],
                               portal_data->fifo_base_virt + msg.channel * 256 + 128);
                }
                mutex_unlock(&portal_data->reg_mutex);
                //dump_regs("PUT", portal_data);
                return 0;
        } break;
	case PORTAL_GET: {
                PortalMessageWithPayload msg;
                int int_status = readl(portal_data->reg_base_virt + 0x00);
                int queue_status = readl(portal_data->reg_base_virt + 0x40);
                int mask = 0;
                if (0)
                printk("%s: GET int_status=%x mask=%x queue_status=%x\n",
                       __FUNCTION__, int_status, mask, queue_status);
                if ((int_status & 1) == 0)
                        return -EAGAIN;
                // we need to know how big the buffer is
		if (copy_from_user(&msg, (void __user *)arg, sizeof(msg)))
			return -EFAULT;

                //dump_regs("GET", portal_data);
                mutex_lock(&portal_data->reg_mutex);
                if (portal_data->fifo_base_virt) {
                        int c;
                        for (c = 0; c < 32; c++) {
                                int i;
                                if ((queue_status & (1 << c)) == 0)
                                        continue;
                                if (0)
                                printk("Reading FIFO %d size at %lx\n",
                                       c, (long)portal_data->fifo_base_phys
                                       + c * 256);
                                msg.pm.channel = c;
                                msg.pm.size = readl(portal_data->fifo_base_virt
                                                    + c * 256) * 4;
                                //printk("msg.size=%d\n", msg.pm.size, count);
                                for (i = 0; i < msg.pm.size/4; i++) {
                                        msg.payload[i] = 
                                                readl(portal_data->fifo_base_virt
                                                      + c * 256 + 128);
                                        //printk("%s: result %x %08x\n", __FUNCTION__, i*4, msg.payload[i]);
                                }
                                break;
                        }
                }
                mutex_unlock(&portal_data->reg_mutex);

                if (copy_to_user((void __user *)arg,
                                 &msg,
                                 sizeof(PortalMessage) + msg.pm.size))
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

/* maps the region containing the fifos and registers */
int portal_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct portal_client *portal_client = filep->private_data;
	struct portal_data *portal_data = portal_client->portal_data;
	unsigned long off = portal_data->reg_base_phys;

	// no reason to ask for anything other than first, second, or both pages
	unsigned long req_len = vma->vm_end - vma->vm_start + (vma->vm_pgoff << PAGE_SHIFT);
        if (!(req_len == (1 << PAGE_SHIFT) || req_len == (2 << PAGE_SHIFT)))
	        return -EINVAL;

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
	  dump_regs(__FUNCTION__, portal_data);

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
	void *reg_base_virt, *fifo_base_virt;
	u32 fifo_base_size[2];
	u32 reg_base_phys, fifo_base_phys;
	int reg_range, fifo_range;
	int status = 0, rc = 0;
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
        memcpy(portal_data->timer_values, init_data->timer_values, sizeof(init_data->timer_values));

	reg_base_phys = reg_res->start;
	reg_range = reg_res->end - reg_res->start;
	reg_base_virt = ioremap_nocache(reg_base_phys, reg_range);
        pr_info("%s reg_base phys %x/%x virt %p\n",
                portal_data->device_name,
                reg_base_phys, reg_range, reg_base_virt);
        portal_data->reg_base_phys = reg_base_phys;
        portal_data->reg_base_virt = reg_base_virt;

	status = of_property_read_u32_array(init_data->pdev->dev.of_node,
					    "fifo", fifo_base_size, 2);
        if (status) {
		driver_devel("failed to get fifo address");
		fifo_base_phys = reg_base_phys + 0x10000;
		fifo_range = 0x10000;
		fifo_base_virt = ioremap_nocache(fifo_base_phys, fifo_range);
	} else {
		fifo_base_phys = fifo_base_size[0];
		fifo_range = fifo_base_size[1];
		fifo_base_virt = ioremap_nocache(fifo_base_phys, fifo_range);
	}
        pr_info("%s fifo_base phys %x/%x virt %p\n",
                portal_data->device_name,
                fifo_base_phys, fifo_range, fifo_base_virt);
        portal_data->fifo_base_phys = fifo_base_phys;
        portal_data->fifo_base_virt = fifo_base_virt;

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
