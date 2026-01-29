/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/*
 * coqos-vhost-gadget: vhost transport for the USB gadget subsystem
 */
#include "usb_gadget.h"
#include <linux/usb/composite.h>

#define DRIVER_DESC "Vhost USB gadget"
#define DRIVER_NAME "vhost-usb-gadget"

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)
#define usb_gadget_register_driver(driver) usb_gadget_probe_driver(driver)
#endif

MODULE_DESCRIPTION(DRIVER_DESC);

static int is_vhg_state_and_ep_valid(struct vhost_gadget *vhg,
				     unsigned int ep_id)
{
	if (!vhg->running)
		return -EINVAL;

	if (!vhg->gadget || ep_id >= vhg->nendpoints)
		return -EBUSY;
	return 0;
}

void vhost_usb_data_complete(struct vhost_usb_req *vreq)
{
	struct usb_request *req = vreq->ureq;
	struct vhost_usb *vhu = vreq->vhu;
	struct vhost_virtqueue *vq = &vhu->vqs[VHOST_GADGET_VQ_DATA];
	u32 gadget_idx = le16_to_cpu(vreq->req->port);
	struct vhost_gadget *vg = &vhu->gadgets[gadget_idx];
	u16 endpoint = le16_to_cpu(vreq->req->endpoint);
	u16 ep_id = endpoint & VIRTIO_USB_EP_ID_MASK;
	struct vhg_ep *vep = &vg->veps[ep_id];
	struct iov_iter in_iter;
	size_t nbytes = 0;

	vreq->resp->status = cpu_to_le32(usb_to_virtio_error(req->status));
	vreq->resp->actual_length = cpu_to_le32(req->actual);

	iov_iter_init(&in_iter, ITER_DEST, vreq->data_resp_iov,
		      vreq->data_in_iovs, vreq->in_size);
	vhost_disable_notify(&vhu->vhdev, vq);

	nbytes = copy_to_iter(vreq->resp, vreq->in_size, &in_iter);
	if (nbytes != vreq->in_size) {
		vq_err(vq,
		       "Faulted on copying data response, nbytes written = %lu\n",
		       nbytes);
		goto on_exit;
	}
	vhost_add_used_and_signal(&vhu->vhdev, vq, vreq->vq_desc, nbytes);

on_exit:
	usb_ep_free_request(vep->ep, vreq->ureq);
	vreq->ureq = NULL;
}

static void vhost_gadget_req_complete(struct usb_ep *ep,
				      struct usb_request *req)
{
	struct vhost_usb_req *vreq = req->context;
	struct vhost_usb *vhu = vreq->vhu;
	u32 gadget_idx = le16_to_cpu(vreq->req->port);
	struct vhost_gadget *vhg = &vhu->gadgets[gadget_idx];

	spin_lock(&vhg->lock);
	list_del_init(&vreq->list);
	spin_unlock(&vhg->lock);

	llist_add(&vreq->data_completion_list, &vhu->vhu_data_list);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	vhost_vq_work_queue(&vhu->vqs[VHOST_GADGET_VQ_DATA],
			    &vhu->vhu_data_work);
#else
	vhost_work_queue(&vhu->vhdev, &vhu->vhu_data_work);
#endif
}

static void vhost_gadget_ep0_complete(struct usb_ep *ep,
				      struct usb_request *req)
{
	struct vhost_usb_req *vreq = req->context;
	struct vhost_usb *vhu = vreq->vhu;
	u32 gadget_idx = le16_to_cpu(vreq->req->port);
	struct vhost_gadget *vg = &vhu->gadgets[gadget_idx];

	spin_lock(&vg->lock);
	list_del_init(&vreq->list);

	if (vg->ep0_req_queued)
		vg->ep0_req_queued = false;
	if (vg->ep0_in_pending)
		vg->ep0_in_pending = false;
	else
		vg->ep0_out_pending = false;

	spin_unlock(&vg->lock);

	vreq->resp->status = cpu_to_le32(usb_to_virtio_error(req->status));
	if (!req->status)
		vreq->resp->actual_length = cpu_to_le32(req->actual);

	complete(&vg->ep0_done);
}

static int vhost_gadget_ep0_queue(struct vhost_gadget *vhg,
				  struct vhost_usb_req *vreq,
				  unsigned int direction,
				  unsigned int transfer_flags)
{
	struct vhg_ep *vep = &vhg->veps[0];
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&vhg->lock, flags);
	ret = is_vhg_state_and_ep_valid(vhg, 0);
	if (ret) {
		dev_err(&vhg->gadget->dev, "Invalid state of device\n");
		goto out_unlock;
	}
	if (vhg->ep0_req_queued) {
		dev_dbg(&vhg->gadget->dev,
			"ep0 queue failed, urb already queued\n");
		ret = -EBUSY;
		goto out_unlock;
	}
	if ((direction == VIRTIO_USB_DIR_IN && !vhg->ep0_in_pending) ||
	    (direction == VIRTIO_USB_DIR_OUT && !vhg->ep0_out_pending)) {
		dev_dbg(&vhg->gadget->dev,
			"ep0 queue failed, wrong direction\n");
		ret = -EBUSY;
		goto out_unlock;
	}
	if (WARN_ON(direction == VIRTIO_USB_DIR_IN && vhg->ep0_out_pending)) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (WARN_ON(direction == VIRTIO_USB_DIR_OUT && vhg->ep0_in_pending)) {
		ret = -ENODEV;
		goto out_unlock;
	}
	vhg->ep0_req_queued = true;
	vreq->ureq = vhg->req;
	vreq->ureq->buf = vreq->buf;
	vreq->ureq->context = vreq;
	vreq->ureq->length = vreq->buf_size;
	vreq->ureq->zero = transfer_flags & VIRTIO_USB_FLAG_ZERO_PACKET;
	vreq->ureq->complete = vhost_gadget_ep0_complete;

	list_add_tail(&vreq->list, &vep->req_queue);
	spin_unlock_irqrestore(&vhg->lock, flags);

	ret = usb_ep_queue(vep->ep, vreq->ureq, GFP_KERNEL);
	if (ret) {
		dev_err(&vhg->gadget->dev, "failed, usb_ep_queue returned %d\n",
			ret);
		goto out_queue_failed;

	} else {
		ret = wait_for_completion_interruptible(&vhg->ep0_done);
		if (ret) {
			dev_dbg(&vhg->gadget->dev,
				"wait for ep0 completion interrupted %d\n",
				ret);
			usb_ep_dequeue(vhg->gadget->ep0, vhg->req);
			wait_for_completion(&vhg->ep0_done);
		}
	}
	return ret;

out_queue_failed:
	spin_lock_irqsave(&vhg->lock, flags);
	list_del(&vreq->list);
	vhg->ep0_req_queued = false;

out_unlock:
	vreq->resp->status = cpu_to_le32(usb_to_virtio_error(ret));
	spin_unlock_irqrestore(&vhg->lock, flags);

	return ret;
}

int vhost_gadget_ep_queue(struct vhost_gadget *vhg, struct vhost_usb_req *vreq)
{
	u16 transfer_flags = le16_to_cpu(vreq->req->transfer_flags);
	u16 endpoint = le16_to_cpu(vreq->req->endpoint);
	u16 ep_id = endpoint & VIRTIO_USB_EP_ID_MASK;
	struct vhg_ep *vep = &vhg->veps[ep_id];
	unsigned long flags;
	u16 direction;
	int ret;

	if (!ep_id) {
		direction = (endpoint & VIRTIO_USB_EP_DIR_IN) ?
				    VIRTIO_USB_DIR_IN :
				    VIRTIO_USB_DIR_OUT;
		ret = vhost_gadget_ep0_queue(vhg, vreq, direction,
					     transfer_flags);
		return ret;
	}

	spin_lock_irqsave(&vhg->lock, flags);

	ret = is_vhg_state_and_ep_valid(vhg, ep_id);
	if (ret) {
		dev_err_ratelimited(&vhg->gadget->dev,
				    "Invalid state of device\n");
		goto out_unlock;
	}
	if (!vep->enabled) {
		dev_dbg(&vhg->gadget->dev, "fail, %s is not enabled\n",
			vep->ep->name);
		ret = -ESHUTDOWN;
		goto out_unlock;
	}
	if (vep->disabling) {
		dev_dbg(&vhg->gadget->dev,
			"fail, %s is already being disabled\n", vep->ep->name);
		ret = -ESHUTDOWN;
		goto out_unlock;
	}
	if ((endpoint & USB_DIR_IN) && !usb_endpoint_dir_in(vep->ep->desc)) {
		dev_dbg(&vhg->gadget->dev,
			"fail, wrong direction of endpoint %s\n",
			vep->ep->name);
		ret = -EINVAL;
		goto out_unlock;
	}
	vreq->ureq = usb_ep_alloc_request(vep->ep, GFP_ATOMIC);
	if (!vreq->ureq) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	vreq->ureq->context = vreq;
	vreq->ureq->length = vreq->buf_size;
	vreq->ureq->buf = vreq->buf;
	vreq->ureq->zero = transfer_flags & VIRTIO_USB_FLAG_ZERO_PACKET;
	vreq->ureq->complete = vhost_gadget_req_complete;

	list_add_tail(&vreq->list, &vep->req_queue);
	spin_unlock_irqrestore(&vhg->lock, flags);

	ret = usb_ep_queue(vep->ep, vreq->ureq, GFP_KERNEL);
	if (ret) {
		dev_err_ratelimited(&vhg->gadget->dev,
				    "fail, usb_ep_queue for %s returned %d\n",
				    vep->ep->name, ret);
		goto out_queue_failed;
	}
	return ret;

out_queue_failed:
	spin_lock_irqsave(&vhg->lock, flags);
	list_del(&vreq->list);
	usb_ep_free_request(vep->ep, vreq->ureq);
out_unlock:
	vreq->resp->status = cpu_to_le32(usb_to_virtio_error(ret));
	spin_unlock_irqrestore(&vhg->lock, flags);

	return ret;
}

int vhost_gadget_ep_dequeue(struct vhost_gadget *vhg, u16 endpoint, u64 tag)
{
	u16 ep_id = endpoint & VIRTIO_USB_EP_ID_MASK;
	struct vhost_usb_req *vreq;
	unsigned long flags;
	struct vhg_ep *vep;
	int rc = -EINVAL;

	if (ep_id >= vhg->nendpoints)
		return rc;

	vep = &vhg->veps[ep_id];
	spin_lock_irqsave(&vhg->lock, flags);
	list_for_each_entry(vreq, &vep->req_queue, list) {
		if (le64_to_cpu(vreq->req->tag) == tag) {
			rc = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&vhg->lock, flags);
	if (rc)
		/* Request already completed */
		return 0;

	rc = usb_ep_dequeue(vep->ep, vreq->ureq);
	return rc;
}

static unsigned int vhost_gadget_ep_type(struct usb_ep_caps *caps)
{
	unsigned int type = 0;

	if (caps->type_control)
		type |= 1 << VIRTIO_USB_EP_CONTROL;
	if (caps->type_iso)
		type |= 1 << VIRTIO_USB_EP_ISOCHRONOUS;
	if (caps->type_bulk)
		type |= 1 << VIRTIO_USB_EP_BULK;
	if (caps->type_int)
		type |= 1 << VIRTIO_USB_EP_INTERRUPT;

	return type;
}

static unsigned int vhost_gadget_ep_dir(struct usb_ep_caps *caps)
{
	unsigned int direction = 0;

	if (caps->dir_in)
		direction |= 1 << VIRTIO_USB_DIR_IN;
	if (caps->dir_out)
		direction |= 1 << VIRTIO_USB_DIR_OUT;

	return direction;
}

/**
 * vhost_gadget_bind() - Gadget bind callback.
 * Gadget is registered and deactivated. It will be seen by USB HOST
 * when it is pullup.
 * @gadget: The gadget which has bounded to the UDC
 * @driver: The USB gadget driver structure.
 *
 * Context: Any context.
 * Return: 0 on success, -errno on failure.
 */
static int vhost_gadget_bind(struct usb_gadget *gadget,
			     struct usb_gadget_driver *driver)
{
	struct vhost_gadget *vhg =
		container_of(driver, struct vhost_gadget, driver);
	struct usb_request *req = NULL;
	unsigned int i = 0, neps = 0;
	unsigned long flags;
	struct usb_ep *ep;
	int rc;

	rc = usb_gadget_deactivate(gadget);

	set_gadget_data(gadget, vhg);

	gadget_for_each_ep(ep, gadget) {
		neps++;
	}

	vhg->nendpoints = neps + 1; // additional for ep0
	vhg->epinfo =
		kcalloc(vhg->nendpoints, sizeof(*vhg->epinfo), GFP_KERNEL);
	if (!vhg->epinfo)
		return -ENOMEM;

	vhg->veps = kcalloc(vhg->nendpoints, sizeof(*vhg->veps), GFP_KERNEL);
	if (!vhg->veps) {
		kfree(vhg->epinfo);
		return -ENOMEM;
	}

	req = usb_ep_alloc_request(gadget->ep0, GFP_KERNEL);
	if (!req) {
		set_gadget_data(gadget, NULL);
		kfree(vhg->epinfo);
		kfree(vhg->veps);
		return -ENOMEM;
	}

	spin_lock_irqsave(&vhg->lock, flags);
	vhg->gadget = gadget;
	vhg->req = req;
	vhg->req->context = vhg;
	vhg->req->complete = vhost_gadget_req_complete;
	/* EP0 */
	vhg->veps[i].ep = gadget->ep0;
	vhg->veps[i].ep_id = i;
	vhg->veps[i].enabled = 1;
	vhg->epinfo[i].types = cpu_to_le16(1 << VIRTIO_USB_EP_CONTROL);
	vhg->epinfo[i].directions =
		cpu_to_le16(1 << VIRTIO_USB_DIR_IN | 1 << VIRTIO_USB_DIR_OUT);
	vhg->epinfo[i].maxpacket_limit = cpu_to_le16(gadget->ep0->maxpacket);
	vhg->epinfo[i].max_streams = cpu_to_le16(gadget->ep0->max_streams);
	strscpy(vhg->epinfo[i].name, "ep0", sizeof(vhg->epinfo[i].name));
	INIT_LIST_HEAD(&vhg->veps[i].req_queue);
	i++;
	/* Remaining endpoints */
	gadget_for_each_ep(ep, gadget) {
		vhg->veps[i].ep = ep;
		vhg->veps[i].ep_id = i;
		vhg->veps[i].enabled = 0;
		vhg->epinfo[i].types =
			cpu_to_le16(vhost_gadget_ep_type(&ep->caps));
		vhg->epinfo[i].directions =
			cpu_to_le16(vhost_gadget_ep_dir(&ep->caps));
		vhg->epinfo[i].maxpacket_limit = cpu_to_le16(ep->maxpacket);
		vhg->epinfo[i].max_streams = cpu_to_le16(ep->max_streams);
		strscpy(vhg->epinfo[i].name, ep->name,
			sizeof(vhg->epinfo[i].name));
		INIT_LIST_HEAD(&vhg->veps[i].req_queue);
		i++;
	}
	spin_unlock_irqrestore(&vhg->lock, flags);

	rc = vhost_gadget_send_evt(vhg, VIRTIO_USB_EVT_DEV_CONNECTED, NULL);
	return rc;
}

/**
 * vhost_gadget_unbind() - This callback is called when the gadget driver
 * is unbound from the UDC.
 * @gadget: The gadget which is unbound to the UDC
 *
 * Context: Context that permit sleeping.
 */
static void vhost_gadget_unbind(struct usb_gadget *gadget)
{
	struct vhost_gadget *vhg = get_gadget_data(gadget);
	unsigned int i;

	if (vhg->req) {
		usb_ep_free_request(gadget->ep0, vhg->req);
		vhg->req = NULL;
	}
	for (i = 1; i < vhg->nendpoints; i++) {
		if (!vhg->veps[i].enabled)
			continue;

		kfree(vhg->veps[i].ep->desc);
		vhg->veps[i].enabled = 0;
	}

	vhg->nendpoints = 0;
	kfree(vhg->epinfo);
	kfree(vhg->veps);

	set_gadget_data(gadget, NULL);
}

/**
 * vhost_gadget_setup() - This callback handles USB control requests.
 * @gadget: The gadget which got the setup event
 * @ctrl: The USB control request SETUP data
 *
 * Context: Interrupt context.
 * Return: 0 on success, -errno on failure.
 */
static int vhost_gadget_setup(struct usb_gadget *gadget,
			      const struct usb_ctrlrequest *ctrl)
{
	struct vhost_gadget *vhg = get_gadget_data(gadget);
	int ret;

	spin_lock(&vhg->lock);

	if (vhg->ep0_in_pending || vhg->ep0_out_pending) {
		dev_err(&vhg->gadget->dev,
			"ep0 stalling, request already pending\n");
		ret = -EBUSY;
		goto on_unlock;
	}
	if ((ctrl->bRequestType & USB_DIR_IN))
		vhg->ep0_in_pending = true;
	else
		vhg->ep0_out_pending = true;
	spin_unlock(&vhg->lock);

	ret = vhost_gadget_send_evt(vhg, VIRTIO_USB_EVT_DEV_SETUP,
				    (unsigned char *)ctrl);
	goto on_exit;

on_unlock:
	spin_unlock(&vhg->lock);
on_exit:

	if (ret == 0 && ctrl->wLength == 0) {
		/*
		 * Return USB_GADGET_DELAYED_STATUS as a workaround to stop
		 * some UDC drivers (e.g. dwc3) from automatically proceeding
		 * with the status stage for 0-length transfers.
		 * Should be removed once all UDC drivers are fixed to always
		 * delay the status stage until a response is queued to EP0.
		 */
		return USB_GADGET_DELAYED_STATUS;
	}
	return ret;
}

/**
 * vhost_gadget_disconnect(): Gadget disconnect callback
 * This callback is invoked when the gadget is disconnected from the host.
 * @gadget: The gadget which got disconnected
 *
 * Context: Interrupt Context.
 */
static void vhost_gadget_disconnect(struct usb_gadget *gadget)
{
	struct vhost_gadget *vhg = get_gadget_data(gadget);

	vhost_gadget_send_evt(vhg, VIRTIO_USB_EVT_DEV_DISCONNECTED, NULL);
}

/**
 * vhost_gadget_suspend(): Gadget suspend callback
 * This callback is invoked when the gadget is suspended by the host.
 * @gadget: The gadget which has suspended
 *
 * Context: Interrupt Context.
 */
static void vhost_gadget_suspend(struct usb_gadget *gadget)
{
	struct vhost_gadget *vhg = get_gadget_data(gadget);

	vhost_gadget_send_evt(vhg, VIRTIO_USB_EVT_DEV_SUSPEND, NULL);
}

/**
 * vhost_gadget_resume(): Gadget resume callback
 * This callback is invoked when the gadget is resumed by the host.
 * @gadget: The gadget which has resumed
 *
 * Context: Interrupt Context.
 */
static void vhost_gadget_resume(struct usb_gadget *gadget)
{
	struct vhost_gadget *vhg = get_gadget_data(gadget);

	vhost_gadget_send_evt(vhg, VIRTIO_USB_EVT_DEV_RESUME, NULL);
}

/**
 * vhost_gadget_reset(): Gadget reset callback
 * @gadget: The gadget which is reset
 *
 * Context: Interrupt Context.
 */
static void vhost_gadget_reset(struct usb_gadget *gadget)
{
	struct vhost_gadget *vhg = get_gadget_data(gadget);
	unsigned long flags;

	spin_lock_irqsave(&vhg->lock, flags);

	vhg->ep0_in_pending = false;
	vhg->ep0_out_pending = false;

	spin_unlock_irqrestore(&vhg->lock, flags);

	vhost_gadget_send_evt(vhg, VIRTIO_USB_EVT_DEV_RESET, NULL);
}

int vhost_gadget_driver_init(struct vhost_gadget *vhg, const char *device_name,
			     const char *driver_name, u8 speed)
{
	char *udc_driver_name;
	char *udc_device_name;
	int ret;

	pr_info("%s %p %s %s %d\n", __func__, vhg, device_name, driver_name,
		speed);

	udc_device_name = kmalloc(UDC_NAME_MAX_LENGTH, GFP_KERNEL);
	if (!udc_device_name)
		return -ENOMEM;

	ret = strscpy(udc_device_name, device_name, UDC_NAME_MAX_LENGTH);
	if (ret < 0)
		goto on_free_udc_device_name;

	udc_driver_name = kmalloc(UDC_NAME_MAX_LENGTH, GFP_KERNEL);
	if (!udc_driver_name)
		goto on_free_udc_device_name;

	ret = strscpy(udc_driver_name, driver_name, UDC_NAME_MAX_LENGTH);
	if (ret < 0)
		goto on_free_udc_driver_name;

	ret = 0;
	switch (speed) {
	case USB_SPEED_UNKNOWN:
		speed = USB_SPEED_HIGH;
		break;
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
	case USB_SPEED_SUPER:
		break;
	default:
		ret = -EINVAL;
		goto on_free_udc_driver_name;
	}

	vhg->driver.function = DRIVER_DESC;
	vhg->driver.max_speed = speed;
	vhg->driver.setup = vhost_gadget_setup;
	vhg->driver.disconnect = vhost_gadget_disconnect;
	vhg->driver.bind = vhost_gadget_bind;
	vhg->driver.unbind = vhost_gadget_unbind;
	vhg->driver.suspend = vhost_gadget_suspend;
	vhg->driver.resume = vhost_gadget_resume;
	vhg->driver.reset = vhost_gadget_reset;
	vhg->driver.driver.name = udc_driver_name;
	vhg->driver.udc_name = udc_device_name;
	vhg->driver.match_existing_only = 1;

	spin_lock_init(&vhg->lock);
	init_completion(&vhg->ep0_done);

	return 0;

on_free_udc_driver_name:
	kfree(udc_driver_name);

on_free_udc_device_name:
	kfree(udc_device_name);
	return ret;
}

void vhost_gadget_driver_deinit(struct vhost_gadget *vhg)
{
	kfree(vhg->driver.udc_name);
	kfree(vhg->driver.driver.name);
}

int vhost_gadget_driver_start(struct vhost_gadget *vhg)
{
	int ret;

	ret = usb_gadget_register_driver(&vhg->driver);
	if (!ret)
		vhg->driver_start = 1;
	return ret;
}

int vhost_gadget_driver_stop(struct vhost_gadget *vhg)
{
	int ret = 0;

	if (vhg->driver_start)
		ret = usb_gadget_unregister_driver(&vhg->driver);

	vhg->driver_start = 0;

	return ret;
}

int vhost_gadget_pullup(struct vhost_gadget *vhg, unsigned int value)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&vhg->lock, flags);

	if (!vhg->running) {
		rc = -EINVAL;
		goto out_unlock;
	}

	if (!vhg->gadget) {
		dev_err(&vhg->gadget->dev, "fail, gadget is not bound\n");
		rc = -EBUSY;
		goto out_unlock;
	}
	spin_unlock_irqrestore(&vhg->lock, flags);
	if (value)
		rc = usb_gadget_activate(vhg->gadget);
	else
		rc = usb_gadget_deactivate(vhg->gadget);

	return rc;

out_unlock:
	spin_unlock_irqrestore(&vhg->lock, flags);
	return rc;
}

int vhost_gadget_set_self_powered(struct vhost_gadget *vhg, unsigned int value)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&vhg->lock, flags);

	if (!vhg->running) {
		rc = -EINVAL;
		goto out_unlock;
	}

	if (!vhg->gadget) {
		dev_err(&vhg->gadget->dev, "fail, gadget is not bound\n");
		rc = -EBUSY;
		goto out_unlock;
	}
	spin_unlock_irqrestore(&vhg->lock, flags);

	if (value)
		rc = usb_gadget_set_selfpowered(vhg->gadget);
	else
		rc = usb_gadget_clear_selfpowered(vhg->gadget);

	return rc;

out_unlock:
	spin_unlock_irqrestore(&vhg->lock, flags);
	return rc;
}

int vhost_gadget_vbus_draw(struct vhost_gadget *vhg, unsigned int mA)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&vhg->lock, flags);

	if (!vhg->running) {
		dev_err(&vhg->gadget->dev, "Device is not running\n");
		ret = -EINVAL;
		goto out_unlock;
	}
	if (!vhg->gadget) {
		dev_err(&vhg->gadget->dev, "fail, gadget is not bound\n");
		ret = -EBUSY;
		goto out_unlock;
	}
	usb_gadget_vbus_draw(vhg->gadget, 2 * mA);

out_unlock:
	spin_unlock_irqrestore(&vhg->lock, flags);

	return ret;
}

int vhost_gadget_ep_enable(struct vhost_gadget *vhg,
			   struct usb_endpoint_descriptor *desc, u16 endpoint)
{
	u16 ep_id = endpoint & VIRTIO_USB_EP_ID_MASK;
	struct vhg_ep *vep = &vhg->veps[ep_id];
	struct usb_endpoint_descriptor *descr;
	unsigned int enabled = 0;
	unsigned long flags;
	int ret;

	/*
	 * Endpoints with a maxpacket length of 0 can cause crashes in UDC
	 * drivers.
	 */
	if (usb_endpoint_maxp(desc) == 0) {
		dev_err(&vhg->gadget->dev, "fail, bad endpoint maxpacket\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&vhg->lock, flags);

	ret = is_vhg_state_and_ep_valid(vhg, ep_id);
	if (ret) {
		dev_dbg(&vhg->gadget->dev, "bad state of %s\n", vep->ep->name);
		goto out_unlock;
	}

	enabled = vep->enabled;
	if (enabled) {
		dev_dbg(&vhg->gadget->dev, "ep%u-%s is already enabled\n",
			ep_id, vep->ep->name);
		ret = -EINVAL;
		goto out_unlock;
	}

	if (!usb_gadget_ep_match_desc(vhg->gadget, vep->ep, desc, NULL)) {
		dev_dbg(&vhg->gadget->dev, "%s descr does not match\n",
			vep->ep->name);
		ret = -EINVAL;
		goto out_unlock;
	}

	descr = kzalloc(sizeof(*descr), GFP_ATOMIC);
	if (!descr) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	memcpy(descr, desc, sizeof(*descr));
	vep->ep->desc = descr;
	vep->ep->maxpacket = usb_endpoint_maxp(descr);

	ret = usb_ep_enable(vep->ep);
	if (ret < 0) {
		dev_dbg(&vhg->gadget->dev, "fail, usb_ep_enable returned %d\n",
			ret);
		goto out_free;
	}
	vep->enabled = 1;

out_unlock:
	spin_unlock_irqrestore(&vhg->lock, flags);
	return ret;

out_free:
	spin_unlock_irqrestore(&vhg->lock, flags);
	kfree(descr);
	return ret;
}

int vhost_gadget_ep_disable(struct vhost_gadget *vhg, u16 endpoint)
{
	unsigned int ep_id = endpoint & VIRTIO_USB_EP_ID_MASK;
	struct vhg_ep *vep = &vhg->veps[ep_id];
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&vhg->lock, flags);

	ret = is_vhg_state_and_ep_valid(vhg, ep_id);
	if (ret)
		goto out_unlock;

	if (!vep->enabled) {
		dev_err(&vhg->gadget->dev, "%s endpoint is not enabled\n",
			vep->ep->name);
		ret = -EINVAL;
		goto out_unlock;
	}
	if (vep->disabling) {
		dev_err(&vhg->gadget->dev,
			"fail, %s disable already in progress\n",
			vep->ep->name);
		ret = -EINVAL;
		goto out_unlock;
	}
	vep->disabling = 1;
	spin_unlock_irqrestore(&vhg->lock, flags);

	usb_ep_disable(vep->ep);

	spin_lock_irqsave(&vhg->lock, flags);
	vep->enabled = 0;
	vep->disabling = 0;
	kfree(vep->ep->desc);
	vep->ep->desc = NULL;

out_unlock:
	spin_unlock_irqrestore(&vhg->lock, flags);
	return ret;
}

static int vhost_gadget_ep0_set_halt(struct vhost_gadget *vhg,
				     unsigned int value)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&vhg->lock, flags);

	ret = is_vhg_state_and_ep_valid(vhg, 0);
	if (ret)
		goto out_unlock;

	if (vhg->ep0_req_queued) {
		dev_err(&vhg->gadget->dev, "fail, urb already queued\n");
		ret = -EBUSY;
		goto out_unlock;
	}
	if (!vhg->ep0_in_pending && !vhg->ep0_out_pending) {
		dev_err(&vhg->gadget->dev, "fail, no request pending\n");
		ret = -EBUSY;
		goto out_unlock;
	}

	ret = usb_ep_set_halt(vhg->gadget->ep0);
	if (ret < 0)
		dev_err(&vhg->gadget->dev, "fail, ep0 set_halt returned %d\n",
			ret);

	if (vhg->ep0_in_pending)
		vhg->ep0_in_pending = false;
	else
		vhg->ep0_out_pending = false;

out_unlock:
	spin_unlock_irqrestore(&vhg->lock, flags);
	return ret;
}

int vhost_gadget_ep_set_halt(struct vhost_gadget *vhg, u16 endpoint,
			     unsigned int value)
{
	u16 ep_id = endpoint & VIRTIO_USB_EP_ID_MASK;
	struct vhg_ep *vep = &vhg->veps[ep_id];
	unsigned long flags;
	int ret;

	if (!ep_id)
		return vhost_gadget_ep0_set_halt(vhg, value);

	spin_lock_irqsave(&vhg->lock, flags);

	ret = is_vhg_state_and_ep_valid(vhg, ep_id);
	if (ret)
		goto out_unlock;

	if (!vep->enabled || vep->disabling || !list_empty(&vep->req_queue)) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (usb_endpoint_xfer_isoc(vep->ep->desc)) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (value)
		ret = usb_ep_set_halt(vep->ep);
	else
		ret = usb_ep_clear_halt(vep->ep);

out_unlock:
	spin_unlock_irqrestore(&vhg->lock, flags);

	return ret;
}

int vhost_gadget_ep_set_wedge(struct vhost_gadget *vhg, u16 endpoint)
{
	unsigned int ep_id = endpoint & VIRTIO_USB_EP_ID_MASK;
	struct vhg_ep *vep = &vhg->veps[ep_id];
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&vhg->lock, flags);

	ret = is_vhg_state_and_ep_valid(vhg, ep_id);
	if (ret)
		goto out_unlock;

	if (!vep->enabled || vep->disabling || !list_empty(&vep->req_queue)) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (usb_endpoint_xfer_isoc(vep->ep->desc)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = usb_ep_set_wedge(vep->ep);

out_unlock:
	spin_unlock_irqrestore(&vhg->lock, flags);

	return ret;
}

int vhost_gadget_get_frame_number(struct vhost_gadget *vhg, u32 *frame_number)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&vhg->lock, flags);
	if (!vhg->running) {
		rc = -EINVAL;
		goto out_unlock;
	}

	if (!vhg->gadget) {
		rc = -EBUSY;
		goto out_unlock;
	}
	spin_unlock_irqrestore(&vhg->lock, flags);

	rc = usb_gadget_frame_number(vhg->gadget);
	if (rc < 0)
		return rc;

	*frame_number = rc;

	return rc;

out_unlock:
	spin_unlock_irqrestore(&vhg->lock, flags);

	return rc;
}
