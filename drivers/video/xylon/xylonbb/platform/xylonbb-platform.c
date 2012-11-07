/*
 * Xylon logiCVC frame buffer platform driver
 *
 * Author: Xylon d.o.o.
 * e-mail: davor.joja@logicbricks.com
 *
 * This driver was based on skeletonfb.c and other fb video drivers.
 * 2012 Xylon d.o.o.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */


#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/xylonbb_platform.h>
#include "../core/xylonbb.h"


static void xylonbb_get_platform_layer_params(
	struct xylonbb_platform_layer_params *lparams,
	struct xylonbb_layer_fix_data *lfdata, int id)
{
	lfdata->offset = lparams->offset;
	lfdata->buffer_offset = lparams->buffer_offset;
	lfdata->layer_type = lparams->type;
	lfdata->bpp = lparams->bpp;
	lfdata->bpp_virt = lparams->bpp;
	lfdata->alpha_mode = lparams->alpha_mode;
	if (lfdata->layer_type == LOGICVC_ALPHA_LAYER)
		lfdata->alpha_mode = LOGICVC_LAYER_ALPHA;

	switch (lfdata->bpp) {
		case 8:
			if (lfdata->alpha_mode == LOGICVC_PIXEL_ALPHA)
				lfdata->bpp = 16;
			break;
		case 16:
			if (lfdata->alpha_mode == LOGICVC_PIXEL_ALPHA)
				lfdata->bpp = 32;
			break;
	}

	lfdata->layer_fix_info = id;
}

static int xylonbb_platform_probe(struct platform_device *pdev)
{
	struct xylonbb_init_data init_data;
	struct xylonbb_platform_data *pdata;
	int i;

	memset(&init_data, 0, sizeof(struct xylonbb_init_data));

	init_data.pdev = pdev;

	pdata = (struct xylonbb_platform_data *)pdev->dev.platform_data;
	init_data.vmem_base_addr = pdata->vmem_base_addr;
	init_data.vmem_high_addr = pdata->vmem_high_addr;
	init_data.vmode_data.ctrl_reg = pdata->ctrl_reg;
	strcpy(init_data.vmode_data.bb_vmode_name, pdata->vmode);
	init_data.vmode_data.bb_vmode.refresh = 60;
	init_data.layers = pdata->num_layers;
	init_data.active_layer = pdata->active_layer;
	init_data.bg_layer_bpp = pdata->bg_layer_bpp;
	init_data.bg_layer_alpha_mode = pdata->bg_layer_alpha_mode;
	init_data.display_interface_type = pdata->display_interface_type;
	init_data.flags = pdata->flags;
	init_data.vmode_params_set = false;

	for (i = 0; i < init_data.layers; i++) {
		xylonbb_get_platform_layer_params(
			&pdata->layer_params[i],
			&init_data.lfdata[i], i);
		init_data.lfdata[i].width = pdata->row_stride;
		init_data.layer_ctrl_flags[i] = pdata->layer_params[i].ctrl_flags;
	}

	return xylonbb_init_driver(&init_data);
}

static int xylonbb_platform_remove(struct platform_device *pdev)
{
	return xylonbb_deinit_driver(pdev);
}


/* logiCVC parameters for Xylon Zynq-ZC702 2D3D referent design */
static struct xylonbb_platform_layer_params logicvc_0_layer_params[] = {
	{
		.offset = 7290,
		.buffer_offset = 1080,
		.type = LOGICVC_RGB_LAYER,
		.bpp = 32,
		.alpha_mode = LOGICVC_PIXEL_ALPHA,
		.ctrl_flags = 0,
	},
	{
		.offset = 4050,
		.buffer_offset = 1080,
		.type = LOGICVC_RGB_LAYER,
		.bpp = 32,
		.alpha_mode = LOGICVC_LAYER_ALPHA,
		.ctrl_flags = 0,
	},
	{
		.offset = 0,
		.buffer_offset = 1080,
		.type = LOGICVC_RGB_LAYER,
		.bpp = 32,
		.alpha_mode = LOGICVC_LAYER_ALPHA,
		.ctrl_flags = 0,
	},
	{
		.offset = 12960,
		.buffer_offset = 1080,
		.type = LOGICVC_RGB_LAYER,
		.bpp = 8,
		.alpha_mode = LOGICVC_CLUT_32BPP_ALPHA,
		.ctrl_flags = 0,
	},
};

static struct xylonbb_platform_data logicvc_0_platform_data = {
	.layer_params = logicvc_0_layer_params,
	.vmode = "1024x768",
	.ctrl_reg = (CTRL_REG_INIT | LOGICVC_PIX_ACT_HIGH),
	.vmem_base_addr = 0x10000000,
	.vmem_high_addr = 0x1FFFFFFF,
	.row_stride = 2048,
	.num_layers = ARRAY_SIZE(logicvc_0_layer_params),
	.active_layer = 0,
	.bg_layer_bpp = 0,
	.bg_layer_alpha_mode = LOGICVC_LAYER_ALPHA,
	.display_interface_type = (LOGICVC_DI_PARALLEL << 4) | (LOGICVC_DCS_YUV422),
	.flags = 0,
};

static struct resource logicvc_0_resource[] = {
	{
		.start = 0x40030000,
		.end = (0x40030000 + LOGICVC_REGISTERS_RANGE),
		.flags = IORESOURCE_MEM,
	},
	{
		.start = 91,
		.end = 91,
		.flags = IORESOURCE_IRQ,
	},
};

static struct platform_device logicvc_device = {
	.name = DEVICE_NAME,
	.id = 0,
	.dev = {
		.platform_data = &logicvc_0_platform_data,
	},
	.resource = logicvc_0_resource,
	.num_resources = ARRAY_SIZE(logicvc_0_resource),
};


static struct platform_driver xylonbb_driver = {
	.probe = xylonbb_platform_probe,
	.remove = xylonbb_platform_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = DEVICE_NAME,
	},
};


static int __init xylonbb_platform_init(void)
{
	int err;

#ifndef MODULE
	char *option = NULL;
	/*
	 *  For kernel boot options (in 'video=xxxbb:<options>' format)
	 */
	if (bb_get_options(DRIVER_NAME, &option))
		return -ENODEV;
	/* Set internal module parameters */
	xylonbb_get_params(option);
#endif
	err = platform_device_register(&logicvc_device);
	if (err) {
		pr_err("Error xylonbb device registration\n");
		return err;
	}
	err = platform_driver_register(&xylonbb_driver);
	if (err) {
		pr_err("Error xylonbb driver registration\n");
		platform_device_unregister(&logicvc_device);
		return err;
	}

	return 0;
}

static void __exit xylonbb_platform_exit(void)
{
	platform_driver_unregister(&xylonbb_driver);
	platform_device_unregister(&logicvc_device);
}


#ifndef MODULE
late_initcall(xylonbb_platform_init);
#else
module_init(xylonbb_platform_init);
module_exit(xylonbb_platform_exit);
#endif

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
