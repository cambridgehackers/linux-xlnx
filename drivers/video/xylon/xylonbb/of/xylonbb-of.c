/*
 * Xylon logiCVC frame buffer Open Firmware driver
 *
 * Author: Xylon d.o.o.
 * e-mail: davor.joja@logicbricks.com
 *
 * 2012 Xylon d.o.o.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */


#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/of.h>
#include "../core/xylonbb.h"


static void set_ctrl_reg(struct xylonbb_init_data *init_data,
	unsigned long pix_data_invert, unsigned long pix_clk_act_high)
{
}

static int xylonbb_parse_hw_info(struct device_node *np,
	struct xylonbb_init_data *init_data)
{
	u32 const *prop;
	int size;

	return 0;
}

static int xylonbb_of_probe(struct platform_device *pdev)
{
	struct xylonbb_init_data init_data;
	int i, rc;

        driver_devel("xylonbb_of_probe\n");

	memset(&init_data, 0, sizeof(struct xylonbb_init_data));

	init_data.pdev = pdev;

	rc = xylonbb_parse_hw_info(pdev->dev.of_node, &init_data);
        driver_devel("xylonbb_parse_hw_info returned %d\n", rc);
	if (rc)
		return rc;

	return xylonbb_init_driver(&init_data);
}

static int xylonbb_of_remove(struct platform_device *pdev)
{
	return xylonbb_deinit_driver(pdev);
}


static struct of_device_id xylonbb_of_match[] __devinitdata = {
	{ .compatible = "xylon,logibitblt-3.00.a" },
	{/* end of table */},
};
MODULE_DEVICE_TABLE(of, xylonbb_of_match);


static struct platform_driver xylonbb_of_driver = {
	.probe = xylonbb_of_probe,
	.remove = xylonbb_of_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = DEVICE_NAME,
		.of_match_table = xylonbb_of_match,
	},
};


static int __init xylonbb_of_init(void)
{
#ifndef MODULE
	char *option = NULL;
	/* Set internal module parameters */
	xylonbb_get_params(option);
#endif
	if (platform_driver_register(&xylonbb_of_driver)) {
		pr_err("Error xylonbb driver registration\n");
		return -ENODEV;
	}

	return 0;
}

static void __exit xylonbb_of_exit(void)
{
	platform_driver_unregister(&xylonbb_of_driver);
}


#ifndef MODULE
late_initcall(xylonbb_of_init);
#else
module_init(xylonbb_of_init);
module_exit(xylonbb_of_exit);
#endif

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
