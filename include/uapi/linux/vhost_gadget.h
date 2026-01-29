/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only with Linux-syscall-note
 */
#ifndef LINUX_VHOST_USB_GADGET_IF_H
#define LINUX_VHOST_USB_GADGET_IF_H

#include <linux/vhost.h>

/* Maximum length of driver_name/device_name in the vhost_gadget_set_udc struct. */
#define UDC_NAME_MAX_LENGTH 128

struct vhost_gadget_set_udc {
	__u8 port;
	__u8 device_name[UDC_NAME_MAX_LENGTH];
	__u8 driver_name[UDC_NAME_MAX_LENGTH];
	__u8 speed;
};

struct vhost_gadget_set_running {
	__u8 port;
	__u8 running;
};

#define VHOST_USB_INIT _IOW(VHOST_VIRTIO, 0xa0, __u32)

#define VHOST_GADGET_SET_UDC \
	_IOW(VHOST_VIRTIO, 0xa1, struct vhost_gadget_set_udc)

#define VHOST_GADGET_SET_RUNNING \
	_IOW(VHOST_VIRTIO, 0xa2, struct vhost_gadget_set_running)

#endif /* LINUX_VHOST_GADGET_IF_H */
