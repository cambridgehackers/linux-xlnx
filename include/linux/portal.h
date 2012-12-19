/*
 * Generic userspace hardware bridge
 *
 * Author: Jamey Hicks <jamey.hicks@gmail.com>
 *
 * 2012 (c) Jamey Hicks
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#ifndef __LINUX_PORTAL_H__
#define __LINUX_PORTAL_H__

#include <linux/types.h>
#include <linux/ioctl.h>

typedef struct PortalAlloc {
    size_t size;
    unsigned char *kptr;
} PortalAlloc;

typedef struct PortalMessage {
    size_t size;
} PortalMessage;

#define USHW_ALLOC _IOWR('B', 10, PortalAlloc)
#define USHW_PUTGET _IOWR('B', 17, PortalMessage)
#define USHW_PUT _IOWR('B', 18, PortalMessage)
#define USHW_GET _IOWR('B', 19, PortalMessage)
#define USHW_REGS _IOWR('B', 20, PortalMessage)

#endif

