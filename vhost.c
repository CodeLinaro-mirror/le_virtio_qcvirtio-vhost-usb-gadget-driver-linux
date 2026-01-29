/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/*
 * coqos-vhost-gadget: vhost transport for the USB gadget subsystem
 */

#include "usb_gadget.h"

#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/bitmap.h>

struct vhost_usb_evt {
	/* event to be sent to guest */
	union {
		struct virtio_usb_dev_event event;
		struct virtio_usb_dev_setup_event setup;
	};
	/* event list, serviced from vhost worker thread */
	struct llist_node list;
};

struct vhost_usb_cmd {
	/* cmd hdr */
	struct virtio_usb_dev_cmd_hdr req;
	/* cmd status */
	struct virtio_usb_cmd_status resp;
};

enum {
	VHOST_GADGET_FEATURES =
		(VHOST_FEATURES | (1ULL << VIRTIO_USB_F_DEVICE)),
};

#define VHOST_GADGET_MAX_EVENT 128

unsigned int usb_to_virtio_error(int error)
{
	unsigned int status;

	switch (error) {
	case 0:
		status = VIRTIO_USB_S_OK;
		break;
	case -ENOENT:
	case -ECONNRESET:
		/* cancelled */
		status = VIRTIO_USB_S_ERR_CANCELLED;
		break;
		/* device shutdown or removal*/
	case -ENODEV:
	case -ESHUTDOWN:
		status = VIRTIO_USB_S_ERR_NO_DEVICE;
		break;
	case -EPIPE:
		status = VIRTIO_USB_S_ERR_STALL;
		break;
	case -EOVERFLOW:
		status = VIRTIO_USB_S_ERR_OVERFLOW;
		break;
	case -EREMOTEIO:
		/* short packet */
		status = VIRTIO_USB_S_ERR_SHORT_PKT;
		break;
	case -EINVAL:
		status = VIRTIO_USB_S_ERR_BAD_MSG;
		break;
	case -ECOMM:
		status = VIRTIO_USB_S_ERR_DATA_IN;
		break;
	case -ENOSR:
		status = VIRTIO_USB_S_ERR_DATA_OUT;
		break;
	case -EXDEV:
		status = VIRTIO_USB_S_ERR_ISO_XFER;
		break;
	case -EFBIG:
		status = VIRTIO_USB_S_ERR_ISO_BIG;
		break;
	case -ETIME:
	case -EPROTO:
	case -EILSEQ:
	case -ENOMEM:
	case -EBUSY:
	default:
		status = VIRTIO_USB_S_ERR_INTERNAL;
		break;
	}
	return status;
}

static void vhost_usb_cmd_handle_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq =
		container_of(work, struct vhost_virtqueue, poll.work);
	struct vhost_usb *vhu = container_of(vq->dev, struct vhost_usb, vhdev);
	size_t nbytes = 0, nbytes_total = 0;
	int head, pkts = 0, total_len = 0;
	struct iov_iter out_iter, in_iter;
	struct vhost_usb_cmd cmd;
	size_t in_size, out_size;
	unsigned int out, in;
	bool added = false;
	int rc;
	u16 gadget_idx;
	struct vhost_gadget *vhg;

	mutex_lock(&vq->mutex);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
	if (!vq->private_data)
#else
	if (!vhost_vq_get_backend(vq))
#endif
		goto out;

	if (!vq_meta_prefetch(vq))
		goto out;

	vhost_disable_notify(&vhu->vhdev, vq);
	do {
		head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov), &out,
					 &in, NULL, NULL);
		if (head < 0)
			break;

		if (head == vq->num) {
			if (unlikely(vhost_enable_notify(&vhu->vhdev, vq))) {
				vhost_disable_notify(&vhu->vhdev, vq);
				continue;
			}
			break;
		}
		out_size = iov_length(vq->iov, out);
		in_size = iov_length(&vq->iov[out], in);

		iov_iter_init(&out_iter, ITER_SOURCE, vq->iov, out, out_size);
		iov_iter_init(&in_iter, ITER_DEST, &vq->iov[out], in, in_size);

		if (unlikely(in_size < sizeof(cmd.resp))) {
			vq_err(vq,
			       "Command response buf too small, need min %zu bytes got %zu",
			       sizeof(cmd.resp), in_size);
			break;
		} else if (unlikely(out_size < sizeof(cmd.req))) {
			vq_err(vq,
			       "Command request buf too small, need min %zu bytes got %zu",
			       sizeof(cmd.req), out_size);
			break;
		}

		if (unlikely(!copy_from_iter_full(&cmd.req, sizeof(cmd.req),
						  &out_iter))) {
			vq_err(vq, "Faulted on copying command request\n");
			break;
		}

		gadget_idx = le16_to_cpu(cmd.req.port);
		vhg = &vhu->gadgets[gadget_idx];
		switch (cmd.req.code) {
		case VIRTIO_USB_CMD_DEV_GET_ENDPOINT_COUNT: {
			struct virtio_usb_dev_ep_count ep_count;

			ep_count.status.code = cpu_to_le32(VIRTIO_USB_S_OK);
			ep_count.count = cpu_to_le32(vhg->nendpoints);
			nbytes = copy_to_iter(&ep_count, sizeof(ep_count),
					      &in_iter);
			if (nbytes != sizeof(ep_count)) {
				vq_err(vq,
				       "Faulted on copying command request\n");
				break;
			}
			nbytes_total += nbytes;
			break;
		}
		case VIRTIO_USB_CMD_DEV_GET_ENDPOINT_INFO: {
			cmd.resp.code = cpu_to_le32(VIRTIO_USB_S_OK);
			nbytes = copy_to_iter(&cmd.resp, sizeof(cmd.resp),
					      &in_iter);
			if (nbytes != sizeof(cmd.resp)) {
				vq_err(vq,
				       "Faulted on copying command response\n");
				break;
			}
			nbytes_total += nbytes;

			nbytes = copy_to_iter(vhg->epinfo,
					      sizeof(*vhg->epinfo) *
						      vhg->nendpoints,
					      &in_iter);

			if (nbytes != sizeof(*vhg->epinfo) * vhg->nendpoints) {
				vq_err(vq,
				       "Faulted on copying endpoint info\n");
				break;
			}
			nbytes_total += nbytes;
			break;
		}
		case VIRTIO_USB_CMD_DEV_PULLUP:
		case VIRTIO_USB_CMD_DEV_VBUS_DRAW:
		case VIRTIO_USB_CMD_DEV_EP_SET_HALT:
		case VIRTIO_USB_CMD_DEV_SET_SELF_POWERED: {
			struct virtio_usb_dev_cmd_set_value cmd_gen;
			unsigned int value;
			u16 endpoint;

			iov_iter_init(&out_iter, ITER_SOURCE, vq->iov, out,
				      out_size);

			rc = copy_from_iter_full(&cmd_gen, sizeof(cmd_gen),
						 &out_iter);
			if (unlikely(!rc)) {
				vq_err(vq,
				       "Faulted on copying command request\n");
				break;
			}
			switch (cmd.req.code) {
			case VIRTIO_USB_CMD_DEV_PULLUP:
				value = le32_to_cpu(cmd_gen.value);
				rc = vhost_gadget_pullup(vhg, value);
				break;
			case VIRTIO_USB_CMD_DEV_VBUS_DRAW:
				value = le32_to_cpu(cmd_gen.value);
				rc = vhost_gadget_vbus_draw(vhg, value);
				break;
			case VIRTIO_USB_CMD_DEV_EP_SET_HALT:
				value = le32_to_cpu(cmd_gen.value);
				endpoint = le16_to_cpu(cmd.req.endpoint);
				rc = vhost_gadget_ep_set_halt(vhg, endpoint,
							      value);
				break;
			case VIRTIO_USB_CMD_DEV_SET_SELF_POWERED:
				value = le32_to_cpu(cmd_gen.value);
				rc = vhost_gadget_set_self_powered(vhg, value);
				break;
			default:
				break;
			}

			cmd.resp.code = cpu_to_le32(usb_to_virtio_error(rc));

			nbytes = copy_to_iter(&cmd.resp, sizeof(cmd.resp),
					      &in_iter);
			if (nbytes != sizeof(cmd.resp)) {
				vq_err(vq,
				       "Faulted on copying command status\n");
				break;
			}
			nbytes_total += nbytes;
			break;
		}
		case VIRTIO_USB_CMD_DEV_EP_ENABLE: {
			struct usb_endpoint_descriptor desc;
			u16 endpoint;

			rc = copy_from_iter_full(&desc, sizeof(desc),
						 &out_iter);
			if (!rc) {
				cmd.resp.code =
					cpu_to_le32(VIRTIO_USB_S_ERR_INTERNAL);
				goto complete;
			}
			endpoint = le16_to_cpu(cmd.req.endpoint);
			rc = vhost_gadget_ep_enable(vhg, &desc, endpoint);
			cmd.resp.code = cpu_to_le32(usb_to_virtio_error(rc));
complete:
			nbytes = copy_to_iter(&cmd.resp, sizeof(cmd.resp),
					      &in_iter);
			if (nbytes != sizeof(cmd.resp)) {
				vq_err(vq,
				       "Faulted on copying command status\n");
				break;
			}
			nbytes_total += nbytes;
			break;
		}
		case VIRTIO_USB_CMD_DEV_EP_DISABLE: {
			u16 endpoint = le16_to_cpu(cmd.req.endpoint);

			rc = vhost_gadget_ep_disable(vhg, endpoint);
			cmd.resp.code = cpu_to_le32(usb_to_virtio_error(rc));

			nbytes = copy_to_iter(&cmd.resp, sizeof(cmd.resp),
					      &in_iter);
			if (nbytes != sizeof(cmd.resp)) {
				vq_err(vq,
				       "Faulted on copying command status\n");
				break;
			}
			nbytes_total += nbytes;
			break;
		}
		case VIRTIO_USB_CMD_DEV_EP_SET_WEDGE: {
			unsigned int endpoint = le16_to_cpu(cmd.req.endpoint);

			rc = vhost_gadget_ep_set_wedge(vhg, endpoint);
			cmd.resp.code = cpu_to_le32(usb_to_virtio_error(rc));
			nbytes = copy_to_iter(&cmd.resp, sizeof(cmd.resp),
					      &in_iter);
			if (nbytes != sizeof(cmd.resp)) {
				vq_err(vq,
				       "Faulted on copying command status\n");
				break;
			}
			nbytes_total += nbytes;
			break;
		}
		case VIRTIO_USB_CMD_DEV_CANCEL: {
			struct virtio_usb_dev_cmd_cancel cmd_cancel;
			u16 endpoint;
			u64 tag;

			iov_iter_init(&out_iter, ITER_SOURCE, vq->iov, out,
				      out_size);
			if (unlikely(!copy_from_iter_full(&cmd_cancel,
							  sizeof(cmd_cancel),
							  &out_iter))) {
				vq_err(vq,
				       "Faulted on copying the cancel command request\n");
				break;
			}
			endpoint = le16_to_cpu(cmd_cancel.hdr.endpoint);
			tag = le64_to_cpu(cmd_cancel.tag);
			rc = vhost_gadget_ep_dequeue(vhg, endpoint, tag);
			cmd.resp.code = cpu_to_le32(usb_to_virtio_error(rc));
			nbytes = copy_to_iter(&cmd.resp, sizeof(cmd.resp),
					      &in_iter);
			if (nbytes != sizeof(cmd.resp)) {
				vq_err(vq,
				       "Faulted on copying cancel command status\n");
				break;
			}
			nbytes_total += nbytes;
			break;
		}
		case VIRTIO_USB_CMD_DEV_GET_FRAME_NUMBER: {
			struct virtio_usb_dev_frame_number resp;
			u32 frame_number;

			rc = vhost_gadget_get_frame_number(vhg, &frame_number);
			resp.status.code = cpu_to_le32(usb_to_virtio_error(rc));
			if (!rc)
				resp.frame_number = cpu_to_le32(frame_number);
			nbytes = copy_to_iter(&resp, sizeof(resp), &in_iter);
			if (nbytes != sizeof(resp)) {
				vq_err(vq, "Faulted on copying frame number\n");
				break;
			}
			nbytes_total += nbytes;
			break;
		}
		default:
			cmd.resp.code = cpu_to_le32(VIRTIO_USB_S_ERR_INTERNAL);
			nbytes = copy_to_iter(&cmd.resp, sizeof(cmd.resp),
					      &in_iter);
			if (nbytes != sizeof(cmd.resp)) {
				vq_err(vq,
				       "Faulted on copying command status\n");
				break;
			}
			nbytes_total += nbytes;
			break;
		}
		vhost_add_used(vq, head, nbytes_total);
		added = true;
	} while (likely(!vhost_exceeds_weight(vq, ++pkts, total_len)));

	if (added)
		vhost_signal(&vhu->vhdev, vq);

out:
	mutex_unlock(&vq->mutex);
}

static void vhost_usb_free_evt(struct vhost_usb *vhu, struct vhost_usb_evt *evt)
{
	vhu->vhu_events_nr--;
	kfree(evt);
}

static struct vhost_usb_evt *vhost_usb_allocate_evt(struct vhost_usb *vhu,
						    u32 event, u16 port_id,
						    unsigned char *buf)
{
	struct vhost_virtqueue *vq = &vhu->vqs[VHOST_GADGET_VQ_EVT];
	struct virtio_usb_dev_setup_event *setup;
	struct vhost_usb_evt *evt;

	if (vhu->vhu_events_nr > VHOST_GADGET_MAX_EVENT)
		return ERR_PTR(-EAGAIN);

	evt = kzalloc(sizeof(*evt), GFP_ATOMIC);
	if (!evt)
		return ERR_PTR(-ENOMEM);

	evt->event.code = cpu_to_vhost32(vq, event);
	evt->event.port = cpu_to_vhost16(vq, port_id);

	if (event == VIRTIO_USB_EVT_DEV_SETUP) {
		setup = &evt->setup;
		memcpy(setup->setup, buf, 8);
	}
	vhu->vhu_events_nr++;

	return evt;
}

static void vhost_usb_do_evt_work(struct vhost_usb *vhu,
				  struct vhost_usb_evt *evt)
{
	struct vhost_virtqueue *vq = &vhu->vqs[VHOST_GADGET_VQ_EVT];
	struct virtio_usb_dev_event *event = &evt->event;
	struct virtio_usb_dev_event __user *eventp;
	unsigned int out, in;
	int head, ret;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
	if (!vq->private_data)
#else
	if (!vhost_vq_get_backend(vq))
#endif
		return;

again:
	vhost_disable_notify(&vhu->vhdev, vq);
	head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov), &out, &in,
				 NULL, NULL);
	if (head < 0)
		return;

	if (head == vq->num) {
		if (vhost_enable_notify(&vhu->vhdev, vq))
			goto again;
		return;
	}

	if (vq->iov[out].iov_len != sizeof(struct virtio_usb_dev_event)) {
		vq_err(vq, "Expecting virtio_usb_dev_event, got %zu bytes\n",
		       vq->iov[out].iov_len);
		return;
	}

	eventp = vq->iov[out].iov_base;
	ret = __copy_to_user(eventp, event, sizeof(*event));
	if (!ret)
		vhost_add_used_and_signal(&vhu->vhdev, vq, head, 0);
	else
		vq_err(vq, "Faulted on vhost_gadget_send_event\n");
}

static void vhost_usb_evt_work(struct vhost_work *work)
{
	struct vhost_usb *vhu =
		container_of(work, struct vhost_usb, vhu_event_work);
	struct vhost_virtqueue *vq = &vhu->vqs[VHOST_GADGET_VQ_EVT];
	struct vhost_usb_evt *evt, *t;
	struct llist_node *llnode;

	mutex_lock(&vq->mutex);
	llnode = llist_del_all(&vhu->vhu_event_list);
	llist_for_each_entry_safe(evt, t, llnode, list) {
		vhost_usb_do_evt_work(vhu, evt);
		vhost_usb_free_evt(vhu, evt);
	}
	mutex_unlock(&vq->mutex);
}

int vhost_gadget_send_evt(struct vhost_gadget *vhg, unsigned int code,
			  unsigned char *buf)
{
	struct vhost_usb_evt *evt;

	evt = vhost_usb_allocate_evt(vhg->vhu, code, vhg->device_id, buf);
	if (IS_ERR(evt)) {
		pr_err("%s allocate_evt() error\n", __func__);
		return PTR_ERR(evt);
	}

	llist_add(&evt->list, &vhg->vhu->vhu_event_list);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	vhost_vq_work_queue(&vhg->vhu->vqs[VHOST_GADGET_VQ_EVT],
			    &vhg->vhu->vhu_event_work);
#else
	vhost_work_queue(&vhg->vhu->vhdev, &vhg->vhu->vhu_event_work);
#endif

	return 0;
}

static void vhost_usb_event_handle_kick(struct vhost_work *work)
{
	UNUSED_PARAMETER(work);
}

static void vhost_usb_data_work(struct vhost_work *work)
{
	struct vhost_usb *vhu =
		container_of(work, struct vhost_usb, vhu_data_work);
	struct vhost_virtqueue *vq = &vhu->vqs[VHOST_GADGET_VQ_DATA];
	struct vhost_usb_req *vreq, *t;
	struct llist_node *llnode;

	mutex_lock(&vq->mutex);
	llnode = llist_del_all(&vhu->vhu_data_list);
	llist_for_each_entry_safe(vreq, t, llnode, data_completion_list) {
		vhost_usb_data_complete(vreq);
		kfree(vreq->data_resp_iov);
		kfree(vreq);
	}
	mutex_unlock(&vq->mutex);
}

static struct vhost_usb_req *vhost_usb_alloc_req(struct vhost_virtqueue *vq,
						 unsigned int out,
						 unsigned int in)
{
	struct vhost_usb_req *vreq;
	size_t out_size, in_size;
	struct iov_iter out_iter;
	u16 endpoint, ep_id;

	out_size = iov_length(vq->iov, out);
	in_size = iov_length(&vq->iov[out], in);

	vreq = kzalloc(sizeof(*vreq) + out_size + in_size, GFP_KERNEL);
	if (!vreq)
		return NULL;

	vreq->out_size = out_size;
	vreq->in_size = in_size;

	vreq->req = (void *)vreq + sizeof(*vreq);

	iov_iter_init(&out_iter, ITER_SOURCE, vq->iov, out, vreq->out_size);

	INIT_LIST_HEAD(&vreq->list);

	if (unlikely(vreq->in_size < sizeof(*vreq->resp))) {
		vq_err(vq, "Response buf too small, need min %zu bytes got %zu",
		       sizeof(vreq->resp), vreq->in_size);
		goto on_error;
	} else if (unlikely(vreq->out_size < sizeof(*vreq->req))) {
		vq_err(vq, "Request buf too small, need min %zu bytes got %zu",
		       sizeof(*vreq->req), vreq->out_size);
		goto on_error;
	}

	if (copy_from_iter(vreq->req, sizeof(*vreq->req), &out_iter) !=
	    sizeof(*vreq->req)) {
		vq_err(vq, "Faulted on copy data request from user\n");
		goto on_error;
	}

	endpoint = le16_to_cpu(vreq->req->endpoint);
	ep_id = endpoint & VIRTIO_USB_EP_ID_MASK;

	if (endpoint & USB_DIR_IN) {
		vreq->buf_size = vreq->out_size - sizeof(*vreq->req);
		if (vreq->buf_size) {
			vreq->buf = (void *)vreq->req + sizeof(*vreq->req);
			vreq->resp = (void *)vreq->buf + vreq->buf_size;

			if (copy_from_iter(vreq->buf, vreq->buf_size,
					   &out_iter) != vreq->buf_size) {
				vq_err(vq,
				       "Faulted on copy data buffer from user\n");
				goto on_error;
			}
		} else {
			vreq->buf = NULL;
			vreq->resp = (void *)vreq->req + sizeof(*vreq->req);
		}
	} else {
		vreq->buf_size = vreq->in_size - sizeof(*vreq->resp);
		vreq->resp = (void *)vreq->req + sizeof(*vreq->req);
		if (vreq->buf_size)
			vreq->buf = (void *)vreq->resp + sizeof(*vreq->resp);
		else
			vreq->buf = NULL;
	}
	vreq->data_resp_iov =
		kzalloc(sizeof(*vreq->data_resp_iov) * in, GFP_KERNEL);
	if (!vreq->data_resp_iov)
		goto on_error;

	memcpy(vreq->data_resp_iov, &vq->iov[out],
	       sizeof(*vreq->data_resp_iov) * in);

	return vreq;

on_error:
	kfree(vreq);
	return NULL;
}

static void vhost_usb_data_handle_kick(struct vhost_work *work)
{
	struct vhost_virtqueue *vq =
		container_of(work, struct vhost_virtqueue, poll.work);
	struct vhost_usb *vhu = container_of(vq->dev, struct vhost_usb, vhdev);
	int head, pkts = 0;
	unsigned int out, in;
	bool added = false;
	size_t nbytes = 0, total_len = 0;
	int ret;
	struct vhost_usb_req *vreq;
	u16 endpoint = 0, ep_id = 0;
	struct iov_iter in_iter;
	u16 gadget_idx;
	struct vhost_gadget *vhg;

	mutex_lock(&vq->mutex);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
	if (!vq->private_data)
#else
	if (!vhost_vq_get_backend(vq))
#endif
		goto out_unlock;

	if (!vq_meta_prefetch(vq))
		goto out_unlock;

	vhost_disable_notify(&vhu->vhdev, vq);
	do {
		head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov), &out,
					 &in, NULL, NULL);
		if (head < 0)
			break;

		if (head == vq->num) {
			if (unlikely(vhost_enable_notify(&vhu->vhdev, vq))) {
				vhost_disable_notify(&vhu->vhdev, vq);
				continue;
			}
			break;
		}
		vreq = vhost_usb_alloc_req(vq, out, in);
		if (!vreq)
			break;

		vreq->vq_desc = head;
		vreq->vhu = vhu;

		endpoint = le16_to_cpu(vreq->req->endpoint);
		ep_id = endpoint & VIRTIO_USB_EP_ID_MASK;

		gadget_idx = le16_to_cpu(vreq->req->port);
		vhg = &vhu->gadgets[gadget_idx];

		mutex_unlock(&vq->mutex);
		ret = vhost_gadget_ep_queue(vhg, vreq);
		mutex_lock(&vq->mutex);
		if (ret || !ep_id) {
			iov_iter_init(&in_iter, ITER_DEST, vreq->data_resp_iov,
				      vreq->data_in_iovs, vreq->in_size);
			nbytes = copy_to_iter(vreq->resp, vreq->in_size,
					      &in_iter);
			if (nbytes != vreq->in_size) {
				vq_err(vq,
				       "Faulted on copying data response\n");
				kfree(vreq->data_resp_iov);
				kfree(vreq);
				break;
			}
			vhost_add_used(vq, head, nbytes);
			total_len += nbytes;
			added = true;
			kfree(vreq->data_resp_iov);
			kfree(vreq);
		}
	} while (likely(!vhost_exceeds_weight(vq, ++pkts, total_len)));

	if (added)
		vhost_signal(&vhu->vhdev, vq);

out_unlock:

	mutex_unlock(&vq->mutex);
}

static long vhost_usb_init(struct vhost_usb *vhu, __u32 __user *arg)
{
	struct vhost_virtqueue **vqs;
	unsigned int i;

	if (copy_from_user(&vhu->gadgets_count, arg, sizeof(*arg)))
		return -EFAULT;

	pr_info("%s gadgets_count %d\n", __func__, vhu->gadgets_count);

	vhu->gadgets = kmalloc_array(vhu->gadgets_count, sizeof(*vhu->gadgets),
				     GFP_KERNEL | __GFP_ZERO);
	if (!vhu->gadgets)
		return -ENOMEM;

	for (i = 0; i < vhu->gadgets_count; i++) {
		vhu->gadgets[i].device_id = i;
		vhu->gadgets[i].vhu = vhu;
	}

	vhu->vqs[VHOST_GADGET_VQ_CMD].handle_kick = vhost_usb_cmd_handle_kick;
	vhu->vqs[VHOST_GADGET_VQ_EVT].handle_kick = vhost_usb_event_handle_kick;
	vhu->vqs[VHOST_GADGET_VQ_DATA].handle_kick = vhost_usb_data_handle_kick;

	vqs = kmalloc_array(ARRAY_SIZE(vhu->vqs), sizeof(*vqs), GFP_KERNEL);
	if (!vqs) {
		kfree(vhu->gadgets);
		return -ENOMEM;
	}

	for (i = 0; i < ARRAY_SIZE(vhu->vqs); i++)
		vqs[i] = &vhu->vqs[i];

	vhost_work_init(&vhu->vhu_event_work, vhost_usb_evt_work);
	vhost_work_init(&vhu->vhu_data_work, vhost_usb_data_work);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
	vhost_dev_init(&vhu->vhdev, vqs, ARRAY_SIZE(vhu->vqs), UIO_MAXIOV, 256,
		       0);
#else
	vhost_dev_init(&vhu->vhdev, vqs, ARRAY_SIZE(vhu->vqs), UIO_MAXIOV, 256,
		       0, true, NULL);
#endif

	vhu->state = 1;
	return 0;
}

static long
vhost_gadget_ioctl_set_udc(struct vhost_usb *vhu,
			   const struct vhost_gadget_set_udc __user *ucmd)
{
	struct vhost_gadget_set_udc cmd = { 0 };
	unsigned long rc = 0;
	u16 port;
	struct vhost_gadget *vhg;

	if (copy_from_user(&cmd, ucmd, sizeof(cmd)))
		return -EFAULT;

	port = cmd.port;
	pr_info("%s port %d\n", __func__, (int)port);

	if (port > vhu->gadgets_count) {
		pr_err("%s gadgets[%d] access with wrong index[%d]", __func__,
		       vhu->gadgets_count, port);
		return -EINVAL;
	}

	vhg = &vhu->gadgets[port];

	rc = vhost_gadget_driver_init(vhg, (const char *)cmd.device_name,
				      (const char *)cmd.driver_name, cmd.speed);
	if (rc) {
		pr_err("%s vhost_gadget_driver_init() fails\n", __func__);
		return rc;
	}

	vhg->initialized = 1;
	vhu->state = 2;

	return rc;
}

static long vhost_gadget_start(struct vhost_gadget *vhg)
{
	int i, rc = 0;

	for (i = VHOST_GADGET_VQ_CMD; i <= VHOST_GADGET_VQ_DATA && !rc; i++) {
		struct vhost_virtqueue *vq = &vhg->vhu->vqs[i];

		mutex_lock(&vq->mutex);
		if (!vhost_vq_access_ok(vq)) {
			pr_err("%s vhost_vq_access_ok fail\n", __func__);
			rc = -EFAULT;
			goto on_unlock;
		}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0)
		if (!vq->private_data) {
			vq->private_data = vhg->vhu;
#else
		if (!vhost_vq_get_backend(vq)) {
			vhost_vq_set_backend(vq, vhg->vhu);
#endif
			rc = vhost_vq_init_access(vq);
		}
on_unlock:
		mutex_unlock(&vq->mutex);
		if (rc)
			return rc;
	}

	return vhost_gadget_driver_start(vhg);
}

static void vhost_gadget_stop(struct vhost_gadget *vhg)
{
	vhost_gadget_driver_stop(vhg);
}

static long vhost_usb_ioctl_get_features(struct vhost_usb *vhu,
					 u64 __user *ufeatures)
{
	u64 features = VHOST_GADGET_FEATURES;

	if (copy_to_user(ufeatures, &features, sizeof(features))) {
		pr_err("%s copy_to_user failed\n", __func__);
		return -EFAULT;
	}

	return 0;
}

static long vhost_usb_ioctl_set_features(struct vhost_usb *vhu,
					 u64 __user *ufeatures)
{
	struct vhost_virtqueue *vq;
	unsigned int i;
	u64 features;

	if (copy_from_user(&features, ufeatures, sizeof(features))) {
		pr_err("%s copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	if (features & ~VHOST_GADGET_FEATURES) {
		pr_err("%s features & ~VHOST_GADGET_FEATURES\n", __func__);
		return -EOPNOTSUPP;
	}

	mutex_lock(&vhu->vhdev.mutex);
	for (i = VHOST_GADGET_VQ_CMD; i <= VHOST_GADGET_VQ_DATA; i++) {
		vq = &vhu->vqs[i];
		mutex_lock(&vq->mutex);
		vq->acked_features = features;
		mutex_unlock(&vq->mutex);
	}
	mutex_unlock(&vhu->vhdev.mutex);

	return 0;
}

static long vhost_gadget_ioctl_set_running(
	struct vhost_usb *vhu,
	const struct vhost_gadget_set_running __user *ucmd)
{
	struct vhost_gadget_set_running cmd = { 0 };
	struct vhost_gadget *vhg;
	long rc = 0;
	u8 port, running;

	if (copy_from_user(&cmd, ucmd, sizeof(cmd))) {
		pr_err("%s copy_from_user err\n", __func__);
		return -EFAULT;
	}

	port = cmd.port;
	if (port > vhu->gadgets_count) {
		pr_err("%s gadgets[%d] access with wrong index[%d]", __func__,
		       vhu->gadgets_count, port);
		return -EINVAL;
	}

	vhg = &vhu->gadgets[port];
	running = cmd.running;

	if (!vhg->initialized) {
		pr_err("%s port %d vhg is not initialized\n", __func__, port);
		return -EINVAL;
	}

	pr_debug("%s port %d running %d\n", __func__, (int)port, (int)running);
	mutex_lock(&vhu->vhdev.mutex);

	if (vhg->running == running)
		goto on_unlock;

	rc = vhost_dev_check_owner(&vhu->vhdev);
	if (rc)
		goto on_unlock;

	if (running)
		rc = vhost_gadget_start(vhg);

	if (!running || rc)
		vhost_gadget_stop(vhg);

	if (!rc)
		vhg->running = running;

on_unlock:
	mutex_unlock(&vhu->vhdev.mutex);
	return rc;
}

static long vhost_usb_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct vhost_usb *vhu = file->private_data;
	void __user *argp = (void __user *)arg;
	long rc = -ENODEV;

	pr_debug("%s cmd:0x%02x --->\n", __func__, _IOC_NR(cmd));

	if (!vhu->state && cmd != VHOST_USB_INIT) {
		pr_err("%s (!vhu->state && cmd != VHOST_USB_INIT)\n", __func__);
		goto error;
	}
	if (vhu->state == 1 && cmd != VHOST_GADGET_SET_UDC) {
		pr_err("%s (vhu->state == 1 && cmd != VHOST_GADGET_SET_UDC)\n",
		       __func__);
		goto error;
	}

	switch (cmd) {
	case VHOST_USB_INIT:
		pr_debug("%s VHOST_USB_INIT\n", __func__);
		rc = vhost_usb_init(vhu, (__u32 __user *)arg);
		break;
	case VHOST_GADGET_SET_UDC:
		pr_debug("%s VHOST_GADGET_SET_UDC\n", __func__);
		rc = vhost_gadget_ioctl_set_udc(vhu, argp);
		break;
	case VHOST_GADGET_SET_RUNNING:
		pr_debug("%s VHOST_GADGET_SET_RUNNING\n", __func__);
		rc = vhost_gadget_ioctl_set_running(vhu, argp);
		break;
	case VHOST_GET_FEATURES:
		pr_debug("%s VHOST_GET_FEATURES\n", __func__);
		rc = vhost_usb_ioctl_get_features(vhu, argp);
		break;
	case VHOST_SET_FEATURES:
		pr_debug("%s VHOST_SET_FEATURES\n", __func__);
		rc = vhost_usb_ioctl_set_features(vhu, argp);
		break;
	default:
		mutex_lock(&vhu->vhdev.mutex);
		rc = vhost_dev_ioctl(&vhu->vhdev, cmd, argp);
		if (rc == -ENOIOCTLCMD)
			rc = vhost_vring_ioctl(&vhu->vhdev, cmd, argp);
		mutex_unlock(&vhu->vhdev.mutex);
	}
error:
	if (rc)
		pr_err("%s cmd:0x%02x <--- %ld\n", __func__, _IOC_NR(cmd), rc);
	else
		pr_debug("%s cmd:0x%02x <--- %ld\n", __func__, _IOC_NR(cmd),
			 rc);
	return rc;
}

static int vhost_usb_open(struct inode *inode, struct file *file)
{
	struct vhost_usb *vhu;

	UNUSED_PARAMETER(inode);

	vhu = kzalloc(sizeof(*vhu), GFP_KERNEL);
	if (!vhu)
		return -ENOMEM;

	file->private_data = vhu;

	vhu->vhu_events_nr = 0;

	return 0;
}

static int vhost_usb_release(struct inode *inode, struct file *file)
{
	struct vhost_usb *vhu;
	struct vhost_dev *vhdev;
	unsigned int i;

	UNUSED_PARAMETER(inode);

	vhu = file->private_data;

	if (!vhu)
		return -ENODEV;
	if (!vhu->gadgets)
		return -ENODEV;

	vhdev = &vhu->vhdev;

	vhost_dev_stop(vhdev);

	for (i = 0; i < vhu->gadgets_count; i++) {
		struct vhost_gadget *vhg = &vhu->gadgets[i];

		if (vhg->initialized) {
			mutex_lock(&vhdev->mutex);
			if (vhg->running)
				vhost_gadget_stop(vhg);
			mutex_unlock(&vhdev->mutex);

			vhost_gadget_driver_deinit(vhg);
		}
	}
	kfree(vhu->gadgets);
	vhost_dev_cleanup(vhdev);
	kfree(vhdev->vqs);

	return 0;
}

static const struct file_operations vhost_gadget_fops = {
	.owner = THIS_MODULE,
	.open = vhost_usb_open,
	.release = vhost_usb_release,
	.unlocked_ioctl = vhost_usb_ioctl,
};

static struct miscdevice vhost_gadget_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "vhost-usb-gadget",
	.fops = &vhost_gadget_fops,
};

static int __init vhost_gadget_init(void)
{
	return misc_register(&vhost_gadget_misc);
};
module_init(vhost_gadget_init);

static void __exit vhost_gadget_exit(void)
{
	misc_deregister(&vhost_gadget_misc);
};
module_exit(vhost_gadget_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vhost transport for the USB gadget");
