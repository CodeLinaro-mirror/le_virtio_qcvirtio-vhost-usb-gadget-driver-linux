/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef LINUX_VHOST_USB_GADGET_H
#define LINUX_VHOST_USB_GADGET_H

#include <vhost.h>
#include <linux/module.h>
//# todo replace with the <linux/virtio_usb.h> after virtio-usb driver merge
#include "include/uapi/linux/virtio_usb.h"
#include <linux/version.h>

#include <linux/uio.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/usb/ch11.h>
#include <linux/usb/gadget.h>

#include "include/uapi/linux/vhost_gadget.h"

#define UNUSED_PARAMETER(_name_) (void)(_name_)

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
#define ITER_SOURCE 1 // == WRITE
#define ITER_DEST 0 // == READ

#endif

#define VIRTIO_USB_EP_ID_MASK 0x7F

enum {
	VHOST_GADGET_VQ_CMD = 3,
	VHOST_GADGET_VQ_EVT,
	VHOST_GADGET_VQ_DATA,
};

struct vhg_ep {
	struct vhost_gadget *vhg;
	struct usb_ep *ep;
	struct usb_request *req;
	struct list_head req_queue;
	unsigned int enabled : 1;
	unsigned int disabling : 1;
	unsigned int ep_id;
};

struct vhost_usb_req {
	struct virtio_usb_request *req;
	struct virtio_usb_response *resp;
	struct vhost_usb *vhu;
	/* Descriptor from vhost_get_vq_desc() for virt_queue segment */
	int vq_desc;
	/* Pointer to response header iovec */
	struct iovec *data_resp_iov;
	/* virtio-usb response incoming iovecs */
	int data_in_iovs;
	size_t out_size;
	size_t in_size;
	size_t buf_size;
	void *buf;
	struct usb_request *ureq;
	struct list_head list;
	/* data list, serviced from vhost worker thread */
	struct llist_node data_completion_list;
};

struct vhost_gadget {
	unsigned short device_id;
	unsigned int initialized : 1;
	unsigned int running : 1;
	unsigned int driver_start : 1;
	struct usb_gadget_driver driver;
	struct usb_gadget *gadget;
	struct usb_request *req;
	struct completion ep0_done;
	unsigned int nendpoints;
	struct vhg_ep *veps;
	struct virtio_usb_dev_ep_info *epinfo;
	bool ep0_in_pending;
	bool ep0_out_pending;
	bool ep0_req_queued;
	struct vhost_usb *vhu;
	spinlock_t lock;
};

struct vhost_usb {
	struct vhost_dev vhdev;
	struct vhost_virtqueue vqs[VIRTIO_USB_VQ_MAX];
	struct vhost_gadget *gadgets;
	unsigned int gadgets_count;
	struct vhost_work vhu_event_work; /* evt injection work item */
	struct llist_head vhu_event_list; /* evt injection queue */
	struct vhost_work vhu_data_work; /* data completion injection work item */
	struct llist_head vhu_data_list; /* data completion injection queue */
	int vhu_events_nr; /* num of pending events, protected by vq->mutex */
	int state;
};

int vhost_gadget_driver_init(struct vhost_gadget *vhg, const char *udc_name,
			     const char *udc_driver_name, u8 speed);

void vhost_gadget_driver_deinit(struct vhost_gadget *vhg);

int vhost_gadget_driver_start(struct vhost_gadget *vhg);
int vhost_gadget_driver_stop(struct vhost_gadget *vhg);

int vhost_gadget_send_evt(struct vhost_gadget *vhg, unsigned int code,
			  unsigned char *buf);
int vhost_gadget_pullup(struct vhost_gadget *vhg, unsigned int value);
int vhost_gadget_set_self_powered(struct vhost_gadget *vhg, unsigned int value);
int vhost_gadget_vbus_draw(struct vhost_gadget *vhg, unsigned int mA);
int vhost_gadget_ep_set_halt(struct vhost_gadget *vhg, u16 endpoint,
			     unsigned int value);
int vhost_gadget_ep_set_wedge(struct vhost_gadget *vhg, u16 endpoint);
int vhost_gadget_ep_enable(struct vhost_gadget *vhg,
			   struct usb_endpoint_descriptor *desc, u16 endpoint);
int vhost_gadget_ep_disable(struct vhost_gadget *vhg, u16 endpoint);
int vhost_gadget_ep_queue(struct vhost_gadget *vhg, struct vhost_usb_req *vreq);
int vhost_gadget_ep_dequeue(struct vhost_gadget *vhg, u16 endpoint, u64 tag);
int vhost_gadget_get_frame_number(struct vhost_gadget *vhg, u32 *frame_number);
unsigned int usb_to_virtio_error(int error);
void vhost_usb_data_complete(struct vhost_usb_req *vreq);
#endif /* LINUX_VHOST_USB_GADGET_H */
