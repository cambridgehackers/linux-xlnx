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
        __u32 rop;
        __u32 op;
        __u32 dst_dma_buf;
        __u32 src_dma_buf;
        size_t dst_offset;
        size_t src_offset;
        __u32 dst_stripe;
        __u32 src_stripe;
        __u32 num_columns;
        __u32 num_rows;
        __u32 timeout;
        __u32 status;
};

#define XYLONBB_IOW(num, dtype)  _IOW('B', num, dtype)
#define XYLONBB_IOR(num, dtype)  _IOR('B', num, dtype)
#define XYLONBB_IOWR(num, dtype) _IOWR('B', num, dtype)
#define XYLONBB_IO(num)          _IO('B', num)

#define XYLONBB_IOC_BITBLIT XYLONBB_IOWR(22, struct xylonbb_params)

#define LOGIBITBLIT_CTRL0_ROFF 0x0000
#define LOGIBITBLIT_CTRL0_SRCLIN (1 << 0)
#define LOGIBITBLIT_CTRL0_DSTLIN (1 << 1)
#define LOGIBITBLIT_CTRL0_START  (1 << 7)
#define LOGIBITBLIT_CTRL0_BUSY   (1 << 7)
#define LOGIBITBLIT_CTRL0_RESET  (1 << 8)

#define LOGIBITBLIT_CTRL1_ROFF 0x0004
#define LOGIBITBLIT_CTRL1_COLFMT_ARGB8888 6

#define LOGIBITBLIT_ROP_ROFF   0x0008
#define LOGIBITBLIT_ROP_BLACK  0x0
#define LOGIBITBLIT_ROP_S      0xC
#define LOGIBITBLIT_ROP_WHITE  0xF
#define LOGIBITBLIT_ROP_PD_S_OVER_D  0x0
#define LOGIBITBLIT_ROP_PD_S_ATOP_D  0x6
#define LOGIBITBLIT_ROP_PD_SG_OVER_D 0xF

#define LOGIBITBLIT_OP_ROFF  0x000C
#define LOGIBITBLIT_OP_MOVE_WITH_ROP         0x2
#define LOGIBITBLIT_OP_TRANSPARENT_MOVE      0x4
#define LOGIBITBLIT_OP_PATTERN_FILL_WITH_ROP 0x6
#define LOGIBITBLIT_OP_PATTERN_FILL          0x7
#define LOGIBITBLIT_OP_PORTER_DUFF           0x8
#define LOGIBITBLIT_OP_SOLID_FILL            0xC

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

#endif

