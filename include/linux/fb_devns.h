#ifndef _LINUX_FB_DEVNS_H
#define _LINUX_FB_DEVNS_H

/*
 * include/linux/fb_devns.h
 *
 * Copyright (c) 2011-2013 Cellrox Ltd. Certain portions are copyrighted by
 * Columbia University. This program is free software licensed under the GNU
 * General Public License Version 2 (GPL 2). You can distribute it and/or
 * modify it under the terms of the GPL 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 * The full GPL 2 License is included in this distribution in the file called
 * COPYING
 *
 * Cellrox can be contacted at oss@cellrox.com
 */

#ifdef __KERNEL__

#ifdef CONFIG_FB_DEV_NS

extern struct fb_info *get_fb_info_ns(struct fb_info *fb_info);
extern void put_fb_info_ns(struct fb_info *fb_info);

extern struct fb_info *fb_virt_to_info(struct fb_info *fb_virt);
extern struct fb_info *fb_virt_to_info_ns(struct fb_info *fb_virt);

extern int track_fb_inode(struct fb_info *fb_info, struct inode *inode);
extern void untrack_fb_inode(struct fb_info *fb_info, struct inode *inode);

#else

#define fb_virt_to_info(fb_info)  fb_info
#define fb_virt_to_info_ns(fb_info)  fb_info

#endif

#endif /* __KERNEL__ */

#endif /* _LINUX_FB_DEVNS_H */
