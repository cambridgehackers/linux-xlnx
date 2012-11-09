/*
 * Xylon logiBITBLIT driver IOCTL parameters
 *
 * Author: Jamey Hicks
 * e-mail: jamey.hicks@nokia.com
 *
 * 2012 (c) Nokia, Inc.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#ifndef __LINUX_XYLON_BB_H__
#define __LINUX_XYLON_BB_H__


#include <linux/types.h>
#include <linux/ioctl.h>

struct xylonbb_params {
        __u32 dst_dma_buf;
        __u32 src_dma_buf;
        size_t dst_offset;
        size_t src_offset;
        __u32 dst_stripe;
        __u32 src_stripe;
        __u32 num_columns;
        __u32 num_rows;
        __u32 status;
};

#define XYLONBB_IOW(num, dtype)  _IOW('B', num, dtype)
#define XYLONBB_IOR(num, dtype)  _IOR('B', num, dtype)
#define XYLONBB_IOWR(num, dtype) _IOWR('B', num, dtype)
#define XYLONBB_IO(num)          _IO('B', num)

#define XYLONBB_IOC_BITBLIT XYLONBB_IOWR(22, struct xylonbb_params)

#endif

