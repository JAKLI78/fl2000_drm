// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define INTR_BUFSIZE 1

struct fl2000_intr {
	struct usb_device *usb_dev;
	struct drm_device *drm;
	u8 *buf;
	struct urb *urb;
	struct work_struct work;
	struct workqueue_struct *work_queue;
};

static void fl2000_intr_work(struct work_struct *work)
{
	struct fl2000_intr *intr = container_of(work, struct fl2000_intr, work);
	int ret;

	if (fl2000_check_interrupt(intr->usb_dev))
		drm_kms_helper_hotplug_event(intr->drm);

	/* Wait for the next interrupt; fails with -EPERM once release poisoned the URB */
	ret = usb_submit_urb(intr->urb, GFP_KERNEL);
	if (ret && ret != -EPERM)
		dev_err(&intr->usb_dev->dev, "Interrupt URB resubmit failed (%d)", ret);
}

static void fl2000_intr_complete(struct urb *urb)
{
	struct fl2000_intr *intr = urb->context;

	switch (urb->status) {
	case 0:
		/* Interrupt processing reads registers over USB, so do it in a work queue */
		queue_work(intr->work_queue, &intr->work);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		/* URB killed or device gone */
		break;
	default:
		dev_err(&intr->usb_dev->dev, "Interrupt message failed (%d)", urb->status);
		/* TODO: Signal fault to system and start shutdown of usb_dev */
		break;
	}
}

static void fl2000_intr_release(struct device *dev, void *res)
{
	struct fl2000_intr *intr = res;

	UNUSED(dev);

	/* Poison first so the work cannot resubmit the URB while being cancelled */
	if (intr->urb)
		usb_poison_urb(intr->urb);
	if (intr->work_queue) {
		cancel_work_sync(&intr->work);
		destroy_workqueue(intr->work_queue);
	}

	usb_free_urb(intr->urb);
	kfree(intr->buf);
}

/**
 * fl2000_intr_create() - interrupt processing context creation
 * @interface:	USB interrupt transfers interface
 *
 * This function is called only on Interrupt interface probe
 *
 * Function initiates USB Interrupt transfers
 *
 * Return: Operation result
 */
struct fl2000_intr *fl2000_intr_create(struct usb_device *usb_dev, struct drm_device *drm)
{
	int ret;
	unsigned int ep;
	struct fl2000_intr *intr;
	struct usb_endpoint_descriptor *desc;
	struct usb_interface *interface = usb_ifnum_to_if(usb_dev, FL2000_USBIF_INTERRUPT);

	/* There's only one altsetting (#0) and one endpoint (#3) in the interrupt interface (#2)
	 * but lets try and "find" it anyway
	 */
	ret = usb_find_int_in_endpoint(interface->cur_altsetting, &desc);
	if (ret) {
		dev_err(&usb_dev->dev, "Cannot find interrupt endpoint");
		return ERR_PTR(ret);
	}
	ep = usb_endpoint_num(desc);

	intr = devres_alloc(&fl2000_intr_release, sizeof(*intr), GFP_KERNEL);
	if (!intr) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt private structure");
		return ERR_PTR(-ENOMEM);
	}
	devres_add(&usb_dev->dev, intr);

	intr->usb_dev = usb_dev;
	intr->drm = drm;
	intr->buf = kmalloc(INTR_BUFSIZE, GFP_KERNEL);
	intr->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!intr->buf || !intr->urb) {
		dev_err(&usb_dev->dev, "Cannot allocate interrupt data");
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}
	usb_fill_int_urb(intr->urb, usb_dev, usb_rcvintpipe(usb_dev, ep), intr->buf,
			 INTR_BUFSIZE, fl2000_intr_complete, intr, desc->bInterval);

	INIT_WORK(&intr->work, &fl2000_intr_work);
	intr->work_queue = create_workqueue("fl2000_interrupt");
	if (!intr->work_queue) {
		dev_err(&usb_dev->dev, "Create interrupt workqueue failed");
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return ERR_PTR(-ENOMEM);
	}

	ret = usb_submit_urb(intr->urb, GFP_KERNEL);
	if (ret) {
		dev_err(&usb_dev->dev, "Interrupt URB submit failed (%d)", ret);
		devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
		return ERR_PTR(ret);
	}

	return intr;
}

void fl2000_intr_destroy(struct usb_device *usb_dev)
{
	devres_release(&usb_dev->dev, fl2000_intr_release, NULL, NULL);
}
