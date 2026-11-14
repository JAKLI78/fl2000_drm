// SPDX-License-Identifier: GPL-2.0
/*
 * (C) Copyright 2017, Fresco Logic, Incorporated.
 * (C) Copyright 2018-2020, Artem Mygaiev
 */

#include "fl2000.h"

#define USB_DRIVER_NAME "fl2000_usb"

#define USB_CLASS_AV		0x10
#define USB_SUBCLASS_AV_CONTROL 0x01
#define USB_SUBCLASS_AV_VIDEO	0x02
#define USB_SUBCLASS_AV_AUDIO	0x03

#define USB_VENDOR_FRESCO_LOGIC 0x1D5C
#define USB_PRODUCT_FL2000	0x2000

#define FL2000_ALL_IFS \
	(BIT(FL2000_USBIF_AVCONTROL) | BIT(FL2000_USBIF_STREAMING) | BIT(FL2000_USBIF_INTERRUPT))

static struct usb_device_id fl2000_id_table[] = {
	{ USB_DEVICE_INTERFACE_CLASS(USB_VENDOR_FRESCO_LOGIC, USB_PRODUCT_FL2000, USB_CLASS_AV) },
	{},
};
MODULE_DEVICE_TABLE(usb, fl2000_id_table);

/* Devices that are independent of interfaces, created for the lifetime of USB device instance */
struct fl2000_devs {
	struct regmap *regmap;
	struct i2c_adapter *adapter;
	struct component_match *match;
	int active_if;
	void *group; /* devres group holding everything above, see fl2000_disconnect() */
};

static struct component_master_ops fl2000_master_ops = {
	.bind = fl2000_drm_bind,
	.unbind = fl2000_drm_unbind,
};

static int fl2000_compare(struct device *dev, void *data)
{
	struct i2c_client *client = i2c_verify_client(dev);
	static const char *const fl2000_supported_bridges[] = {
		"it66121", /* IT66121 driver name*/
	};

	UNUSED(data);

	if (!client)
		return 0;

	/* Check this is a supported DRM bridge */
	for (int i = 0; i < ARRAY_SIZE(fl2000_supported_bridges); i++)
		if (!strncmp(fl2000_supported_bridges[i], client->name, sizeof(client->name)))
			return 1; /* Must be not 0 for success */

	return 0;
}

/* Shared resources live on the USB device, which this driver does not bind to, so they are
 * grouped and released explicitly when the last interface disconnects (e.g. on module unload)
 */
static struct fl2000_devs *fl2000_get_devices(struct usb_device *usb_dev)
{
	struct fl2000_devs *devs;
	void *group;
	int ret;

	group = devres_open_group(&usb_dev->dev, NULL, GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);

	devs = devm_kzalloc(&usb_dev->dev, sizeof(*devs), GFP_KERNEL);
	if (!devs) {
		ret = -ENOMEM;
		goto error;
	}
	devs->group = group;

	devs->regmap = fl2000_regmap_init(usb_dev);
	if (IS_ERR(devs->regmap)) {
		ret = PTR_ERR(devs->regmap);
		goto error;
	}

	devs->adapter = fl2000_i2c_init(usb_dev);
	if (IS_ERR(devs->adapter)) {
		ret = PTR_ERR(devs->adapter);
		goto error;
	}

	component_match_add(&devs->adapter->dev, &devs->match, fl2000_compare, NULL);

	devres_close_group(&usb_dev->dev, group);
	dev_set_drvdata(&usb_dev->dev, devs);

	return devs;

error:
	devres_release_group(&usb_dev->dev, group);
	return ERR_PTR(ret);
}

/* TODO: Halt driver on initialization failure */
static int fl2000_probe(struct usb_interface *interface, const struct usb_device_id *usb_dev_id)
{
	int ret = 0;
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs = dev_get_drvdata(&usb_dev->dev);

	UNUSED(usb_dev_id);

	if (usb_dev->speed < USB_SPEED_HIGH) {
		dev_err(&usb_dev->dev, "USB 1.1 is not supported!");
		return -ENODEV;
	}

	if (!devs) {
		devs = fl2000_get_devices(usb_dev);
		if (IS_ERR(devs)) {
			dev_err(&usb_dev->dev, "Cannot initialize I2C and regmap!");
			return -ENODEV;
		}
	}

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
	case FL2000_USBIF_STREAMING:
	case FL2000_USBIF_INTERRUPT:
		devs->active_if |= BIT(iface_num);
		break;

	default: /* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?", iface_num);
		ret = -ENODEV;
		break;
	}

	/* When all interfaces are up - proceed with registration */
	if (devs->active_if == FL2000_ALL_IFS) {
		ret = component_master_add_with_match(&devs->adapter->dev, &fl2000_master_ops,
						      devs->match);
		if (ret) {
			dev_err(&usb_dev->dev, "Cannot register component master (%d)", ret);
			return ret;
		}
	}

	return ret;
}

static void fl2000_disconnect(struct usb_interface *interface)
{
	u8 iface_num = interface->cur_altsetting->desc.bInterfaceNumber;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct fl2000_devs *devs = dev_get_drvdata(&usb_dev->dev);

	if (!devs)
		return;

	if (devs->active_if == FL2000_ALL_IFS)
		component_master_del(&devs->adapter->dev, &fl2000_master_ops);

	switch (iface_num) {
	case FL2000_USBIF_AVCONTROL:
	case FL2000_USBIF_STREAMING:
	case FL2000_USBIF_INTERRUPT:
		devs->active_if &= ~BIT(iface_num);
		break;

	default: /* Device does not have any other interfaces */
		dev_warn(&interface->dev, "What interface %d?", iface_num);
		break;
	}

	/* Last interface gone: remove the I2C bus and free shared state */
	if (!devs->active_if) {
		void *group = devs->group;

		dev_set_drvdata(&usb_dev->dev, NULL);
		devres_release_group(&usb_dev->dev, group);
	}
}

/* All interfaces belong to one display; handle it once, on the AV control interface. The USB
 * device stays active until all its interfaces are suspended. Without power loss the state is
 * restored on resume; if the device was reset during sleep, the USB core rebinds the driver
 * instead (there is no reset_resume), the same as an unplug and replug
 */
static int fl2000_suspend(struct usb_interface *interface, pm_message_t message)
{
	UNUSED(message);

	if (interface->cur_altsetting->desc.bInterfaceNumber != FL2000_USBIF_AVCONTROL)
		return 0;

	return fl2000_drm_suspend(interface_to_usbdev(interface));
}

static int fl2000_resume(struct usb_interface *interface)
{
	if (interface->cur_altsetting->desc.bInterfaceNumber != FL2000_USBIF_AVCONTROL)
		return 0;

	return fl2000_drm_resume(interface_to_usbdev(interface));
}

/* Without these the USB core would unbind and rebind the driver on a reset, creating a new display
 * device. All interfaces must have them, otherwise the core still rebinds the ones lacking them
 */
static int fl2000_reset_resume(struct usb_interface *interface)
{
	if (interface->cur_altsetting->desc.bInterfaceNumber != FL2000_USBIF_AVCONTROL)
		return 0;

	return fl2000_drm_reset_resume(interface_to_usbdev(interface));
}

static int fl2000_pre_reset(struct usb_interface *interface)
{
	if (interface->cur_altsetting->desc.bInterfaceNumber != FL2000_USBIF_AVCONTROL)
		return 0;

	return fl2000_drm_pre_reset(interface_to_usbdev(interface));
}

static int fl2000_post_reset(struct usb_interface *interface)
{
	if (interface->cur_altsetting->desc.bInterfaceNumber != FL2000_USBIF_AVCONTROL)
		return 0;

	return fl2000_drm_post_reset(interface_to_usbdev(interface));
}

static struct usb_driver fl2000_driver = {
	.name = USB_DRIVER_NAME,
	.probe = fl2000_probe,
	.disconnect = fl2000_disconnect,
	.suspend = fl2000_suspend,
	.resume = fl2000_resume,
	.reset_resume = fl2000_reset_resume,
	.pre_reset = fl2000_pre_reset,
	.post_reset = fl2000_post_reset,
	.id_table = fl2000_id_table,
	.supports_autosuspend = false,
	.disable_hub_initiated_lpm = true,
};

module_usb_driver(fl2000_driver);

MODULE_AUTHOR("Artem Mygaiev");
MODULE_DESCRIPTION("FL2000 USB display driver");
MODULE_LICENSE("GPL v2");
